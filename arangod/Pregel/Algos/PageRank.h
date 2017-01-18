////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_ALGOS_PAGERANK_H
#define ARANGODB_PREGEL_ALGOS_PAGERANK_H 1

#include <velocypack/Slice.h>
#include "Pregel/Algorithm.h"

namespace arangodb {
namespace pregel {
namespace algos {

/// PageRank
struct PageRank : public SimpleAlgorithm<float, float, float> {
  float _threshold;

 public:
  PageRank(arangodb::velocypack::Slice params);

  GraphFormat<float, float>* inputFormat() override {
    return new VertexGraphFormat<float, float>(_resultField, 0);
  }
  
  MessageFormat<float>* messageFormat() const override {
    return new FloatMessageFormat();
  }
  
  MessageCombiner<float>* messageCombiner() const override {
    return new SumCombiner<float>();
  }
  
  VertexComputation<float, float, float>* createComputation(
      WorkerConfig const*) const override;
  Aggregator* aggregator(std::string const& name) const override;
};
}
}
}
#endif