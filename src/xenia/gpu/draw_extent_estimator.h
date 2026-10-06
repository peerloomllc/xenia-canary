/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <cstdint>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader);
  // Runs the vertex shader on the CPU for a draw of at most max_vertices
  // vertices and returns the screen-space rectangle (in guest pixels,
  // including the window offset, widened by a pixel on each side) that its
  // vertices cover. False if that can't be done (too many vertices, points,
  // tessellation, a vertex at or behind the eye, or a shader the interpreter
  // can't run).
  bool EstimateVertexBounds(const Shader& vertex_shader, uint32_t max_vertices,
                            int32_t& left_out, int32_t& top_out,
                            int32_t& right_out, int32_t& bottom_out);

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_x_.reset();
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_x() const { return position_x_; }
    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_x_;
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
