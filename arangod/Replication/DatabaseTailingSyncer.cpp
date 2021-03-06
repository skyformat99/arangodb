////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "DatabaseTailingSyncer.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Logger/Logger.h"
#include "Replication/DatabaseInitialSyncer.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Rest/HttpRequest.h"
#include "RestServer/DatabaseFeature.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Hints.h"
#include "Utils/CollectionGuard.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::httpclient;
using namespace arangodb::rest;

DatabaseTailingSyncer::DatabaseTailingSyncer(
    TRI_vocbase_t* vocbase,
    ReplicationApplierConfiguration const& configuration,
    TRI_voc_tick_t initialTick, bool useTick, TRI_voc_tick_t barrierId)
    : TailingSyncer(vocbase->replicationApplier(),
                    configuration, initialTick, useTick, barrierId),
                    _vocbase(vocbase) {
  _vocbases.emplace(vocbase->name(), DatabaseGuard(vocbase));
  if (configuration._database.empty()) {
    _databaseName = vocbase->name();
  }
}

/// @brief save the current applier state
Result DatabaseTailingSyncer::saveApplierState() {
  LOG_TOPIC(TRACE, Logger::REPLICATION)
      << "saving replication applier state. last applied continuous tick: "
      << applier()->_state._lastAppliedContinuousTick
      << ", safe resume tick: " << applier()->_state._safeResumeTick;

  try {
    _applier->persistState(false);
    return Result();
  } catch (basics::Exception const& ex) {
    std::string errorMsg = std::string("unable to save replication applier state: ") + ex.what();
    LOG_TOPIC(WARN, Logger::REPLICATION) << errorMsg;
    THROW_ARANGO_EXCEPTION_MESSAGE(ex.code(), errorMsg);
  } catch (std::exception const& ex) {
    std::string errorMsg = std::string("unable to save replication applier state: ") + ex.what();
    LOG_TOPIC(WARN, Logger::REPLICATION) << errorMsg;
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, errorMsg);
  } catch (...) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "caught unknown exception while saving applier state");
  }
  return TRI_ERROR_INTERNAL;
}

std::unique_ptr<InitialSyncer> DatabaseTailingSyncer::initialSyncer() {
  TRI_ASSERT(!_configuration._skipCreateDrop);
  return std::make_unique<DatabaseInitialSyncer>(vocbase(), _configuration);
}

/// @brief finalize the synchronization of a collection by tailing the WAL
/// and filtering on the collection name until no more data is available
Result DatabaseTailingSyncer::syncCollectionFinalize(std::string const& collectionName) {
  // fetch master state just once
  Result r = getMasterState();
  if (r.fail()) {
    return r;
  }
  
  // print extra info for debugging
  _configuration._verbose = true;
  // we do not want to apply rename, create and drop collection operations
  _ignoreRenameCreateDrop = true;
  
  TRI_voc_tick_t fromTick = _initialTick;
  LOG_TOPIC(DEBUG, Logger::REPLICATION) << "starting syncCollectionFinalize:"
  << collectionName << ", fromTick " << fromTick;
  
  while (true) {
    if (application_features::ApplicationServer::isStopping()) {
      return Result(TRI_ERROR_SHUTTING_DOWN);
    }
    
    std::string const url = tailingBaseUrl("tail") + "chunkSize=" +
    StringUtils::itoa(_configuration._chunkSize) + "&from=" +
    StringUtils::itoa(fromTick) + "&serverId=" + _localServerIdString +
    "&collection=" + StringUtils::urlEncode(collectionName);
    
    // send request
    std::unique_ptr<SimpleHttpResult> response(_client->request(rest::RequestType::GET, url, nullptr, 0));
    
    if (hasFailed(response.get())) {
      return buildHttpError(response.get(), url);
    }
    
    if (response->getHttpReturnCode() == 204) {
      // HTTP 204 No content: this means we are done
      return Result();
    }
    
    bool found;
    std::string header = response->getHeaderField(TRI_REPLICATION_HEADER_CHECKMORE, found);
    bool checkMore = false;
    if (found) {
      checkMore = StringUtils::boolean(header);
    }
    
    header =
    response->getHeaderField(TRI_REPLICATION_HEADER_LASTINCLUDED, found);
    if (!found) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE, std::string("got invalid response from master at ") +
                    _masterInfo._endpoint + ": required header " + TRI_REPLICATION_HEADER_LASTINCLUDED + " is missing");
    }
    TRI_voc_tick_t lastIncludedTick = StringUtils::uint64(header);
    
    // was the specified from value included the result?
    bool fromIncluded = false;
    header = response->getHeaderField(TRI_REPLICATION_HEADER_FROMPRESENT, found);
    if (found) {
      fromIncluded = StringUtils::boolean(header);
    }
    if (!fromIncluded && fromTick > 0) { // && _requireFromPresent
      return Result(TRI_ERROR_REPLICATION_START_TICK_NOT_PRESENT, std::string("required follow tick value '") +
                    StringUtils::itoa(lastIncludedTick) + "' is not present (anymore?) on master at " +
                    _masterInfo._endpoint + ". Last tick available on master is '" + StringUtils::itoa(lastIncludedTick) +
                    "'. It may be required to do a full resync and increase the number of historic logfiles on the master.");
    }
    
    uint64_t processedMarkers = 0;
    uint64_t ignoreCount = 0;
    Result r = applyLog(response.get(), fromTick, processedMarkers, ignoreCount);
    if (r.fail()) {
      return r;
    }
    
    // update the tick from which we will fetch in the next round
    if (lastIncludedTick > fromTick) {
      fromTick = lastIncludedTick;
    } else if (checkMore) {
      // we got the same tick again, this indicates we're at the end
      checkMore = false;
      LOG_TOPIC(WARN, Logger::REPLICATION) << "we got the same tick again, "
        << "this indicates we're at the end";
    }
    
    if (!checkMore) {
      // done!
      return Result();
    }
    LOG_TOPIC(DEBUG, Logger::REPLICATION) << "Fetching more data fromTick " << fromTick;
  }
}
