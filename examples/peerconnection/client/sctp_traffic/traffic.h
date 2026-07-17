#pragma once
#include "absl/types/span.h"

class Conductor;  // fwd

namespace sctp {

enum class FlowKind {
  kBulk,
  kKv,
  kMesh,
};

class Receiver {
public:
  virtual ~Receiver() = default;
  virtual void Attach(Conductor& c) = 0;   
  virtual void Detach() = 0;              
};

class Sender {
public:
  virtual ~Sender() = default;
  virtual void Start(Conductor& c) = 0;
  virtual void Stop() = 0;
};

} // namespace sctp
