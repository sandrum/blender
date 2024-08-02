/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "BLI_math_angle_types.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "GPU_capabilities.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "COM_algorithm_realize_on_domain.hh"

namespace blender::realtime_compositor {

static const char *get_realization_shader(Result &input,
                                          const RealizationOptions &realization_options)
{
  if (realization_options.interpolation == Interpolation::Bicubic) {
    switch (input.type()) {
      case ResultType::Color:
        return "compositor_realize_on_domain_bicubic_color";
      case ResultType::Vector:
        return "compositor_realize_on_domain_bicubic_vector";
      case ResultType::Float:
        return "compositor_realize_on_domain_bicubic_float";
      case ResultType::Int2:
      case ResultType::Float2:
      case ResultType::Float3:
        /* Realization does not support internal image types. */
        break;
    }
  }
  else {
    switch (input.type()) {
      case ResultType::Color:
        return "compositor_realize_on_domain_color";
      case ResultType::Vector:
        return "compositor_realize_on_domain_vector";
      case ResultType::Float:
        return "compositor_realize_on_domain_float";
      case ResultType::Int2:
      case ResultType::Float2:
      case ResultType::Float3:
        /* Realization does not support internal image types. */
        break;
    }
  }

  BLI_assert_unreachable();
  return nullptr;
}

void realize_on_domain(Context &context,
                       Result &input,
                       Result &output,
                       const Domain &domain,
                       const float3x3 &input_transformation,
                       const RealizationOptions &realization_options)
{
  const Domain input_domain = Domain(input.domain().size, input_transformation);
  if (input_domain == domain) {
    input.pass_through(output);
    output.set_transformation(domain.transformation);
    return;
  }

  GPUShader *shader = context.get_shader(get_realization_shader(input, realization_options));
  GPU_shader_bind(shader);

  /* Translation from lower-left corner to center of input space. */
  float2 input_translate(-float2(input_domain.size) / 2.0f);

  /* Bias translations in case of nearest interpolation to avoids the round-to-even behavior of
   * some GPUs at pixel boundaries. */
  if (realization_options.interpolation == Interpolation::Nearest) {
    input_translate += std::numeric_limits<float>::epsilon() * 10e3f;
  }

  /* Transformation from input domain with 0,0 in lower-left to virtual compositing space. */
  const float3x3 in_transformation = math::translate(input_transformation, input_translate);

  /* Transformation from output domain with 0,0 in lower-left to virtual compositing space. */
  const float3x3 out_transformation = math::translate(domain.transformation, -float2(domain.size) / 2.0f);

  /* Concatenate to get full transform from output space to input space */
  const float3x3 inverse_transformation = math::invert(in_transformation) * out_transformation;

  GPU_shader_uniform_mat3_as_mat4(shader, "inverse_transformation", inverse_transformation.ptr());

  /* The texture sampler should use bilinear interpolation for both the bilinear and bicubic
   * cases, as the logic used by the bicubic realization shader expects textures to use bilinear
   * interpolation. */
  const bool use_bilinear = ELEM(
      realization_options.interpolation, Interpolation::Bilinear, Interpolation::Bicubic);
  GPU_texture_filter_mode(input.texture(), use_bilinear);

  /* If the input wraps, set a repeating wrap mode for out-of-bound texture access. Otherwise,
   * make out-of-bound texture access return zero by setting a clamp to border extend mode. */
  GPU_texture_extend_mode_x(input.texture(),
                            realization_options.wrap_x ? GPU_SAMPLER_EXTEND_MODE_REPEAT :
                                                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER);
  GPU_texture_extend_mode_y(input.texture(),
                            realization_options.wrap_y ? GPU_SAMPLER_EXTEND_MODE_REPEAT :
                                                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER);

  input.bind_as_texture(shader, "input_tx");

  output.allocate_texture(domain);
  output.bind_as_image(shader, "domain_img");

  compute_dispatch_threads_at_least(shader, domain.size);

  input.unbind_as_texture();
  output.unbind_as_image();
  GPU_shader_unbind();
}

}  // namespace blender::realtime_compositor
