// g2o - General Graph Optimization
// Copyright (C) 2019 S. Kasperski
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
// TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//#include <g2o/types/slam3d/edge_se3_prior.h>
//#include <g2o/types/slam3d/isometry3d_gradients.h>
//#include <iostream>

#include "edge_orientation_prior.h"

namespace g2o {
  using namespace std;

  EdgeOrientationPrior::EdgeOrientationPrior(const Quaternion& m, const Isometry3& s)
  : BaseUnaryEdge<3, Quaternion, VertexSE3>() {
    _measurement = m;
    information().setIdentity();
    _sensor_pose = s;
  }

  void EdgeOrientationPrior::computeError() {
    VertexSE3 *vertex = static_cast<VertexSE3*>(_vertices[0]);
    Quaternion current_state(vertex->estimate().linear());
    Isometry3 exp = _sensor_pose.inverse() * _measurement;
    Quaternion expected_state(exp.linear());
    Quaternion err = expected_state.inverse() * current_state;
    _error(0) = err.x();
    _error(1) = err.y();
    _error(2) = err.z();
  }

  bool EdgeOrientationPrior::read(std::istream& is) {
    return false;
  }

  bool EdgeOrientationPrior::write(std::ostream& os) const {
    return false;
  }
}
