/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2023 Blender Foundation */

/** \file
 * \ingroup gpu
 */

#include "vk_command_buffer.hh"
#include "vk_buffer.hh"
#include "vk_context.hh"
#include "vk_framebuffer.hh"
#include "vk_memory.hh"
#include "vk_pipeline.hh"
#include "vk_texture.hh"

#include "BLI_assert.h"

namespace blender::gpu {

VKCommandBuffer::~VKCommandBuffer()
{
  if (vk_device_ != VK_NULL_HANDLE) {
    VK_ALLOCATION_CALLBACKS;
    vkDestroyFence(vk_device_, vk_fence_, vk_allocation_callbacks);
    vk_fence_ = VK_NULL_HANDLE;
  }
}

void VKCommandBuffer::init(const VkDevice vk_device,
                           const VkQueue vk_queue,
                           VkCommandBuffer vk_command_buffer)
{
  vk_device_ = vk_device;
  vk_queue_ = vk_queue;
  vk_command_buffer_ = vk_command_buffer;
  submission_id_.reset();

  if (vk_fence_ == VK_NULL_HANDLE) {
    VK_ALLOCATION_CALLBACKS;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vk_device_, &fenceInfo, vk_allocation_callbacks, &vk_fence_);
  }
}

void VKCommandBuffer::begin_recording()
{
  vkWaitForFences(vk_device_, 1, &vk_fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(vk_device_, 1, &vk_fence_);
  vkResetCommandBuffer(vk_command_buffer_, 0);

  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(vk_command_buffer_, &begin_info);
}

void VKCommandBuffer::end_recording()
{
  vkEndCommandBuffer(vk_command_buffer_);
}

void VKCommandBuffer::bind(const VKPipeline &pipeline, VkPipelineBindPoint bind_point)
{
  vkCmdBindPipeline(vk_command_buffer_, bind_point, pipeline.vk_handle());
}

void VKCommandBuffer::bind(const VKDescriptorSet &descriptor_set,
                           const VkPipelineLayout vk_pipeline_layout,
                           VkPipelineBindPoint bind_point)
{
  VkDescriptorSet vk_descriptor_set = descriptor_set.vk_handle();
  vkCmdBindDescriptorSets(
      vk_command_buffer_, bind_point, vk_pipeline_layout, 0, 1, &vk_descriptor_set, 0, 0);
}

void VKCommandBuffer::begin_render_pass(const VKFrameBuffer &framebuffer)
{
  VkRenderPassBeginInfo render_pass_begin_info = {};
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.renderPass = framebuffer.vk_render_pass_get();
  render_pass_begin_info.framebuffer = framebuffer.vk_framebuffer_get();
  render_pass_begin_info.renderArea = framebuffer.vk_render_area_get();
  vkCmdBeginRenderPass(vk_command_buffer_, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void VKCommandBuffer::end_render_pass(const VKFrameBuffer & /*framebuffer*/)
{
  vkCmdEndRenderPass(vk_command_buffer_);
}

void VKCommandBuffer::push_constants(const VKPushConstants &push_constants,
                                     const VkPipelineLayout vk_pipeline_layout,
                                     const VkShaderStageFlags vk_shader_stages)
{
  BLI_assert(push_constants.layout_get().storage_type_get() ==
             VKPushConstants::StorageType::PUSH_CONSTANTS);
  vkCmdPushConstants(vk_command_buffer_,
                     vk_pipeline_layout,
                     vk_shader_stages,
                     push_constants.offset(),
                     push_constants.layout_get().size_in_bytes(),
                     push_constants.data());
}

void VKCommandBuffer::fill(VKBuffer &buffer, uint32_t clear_data)
{
  vkCmdFillBuffer(vk_command_buffer_, buffer.vk_handle(), 0, buffer.size_in_bytes(), clear_data);
}

void VKCommandBuffer::copy(VKBuffer &dst_buffer,
                           VKTexture &src_texture,
                           Span<VkBufferImageCopy> regions)
{
  vkCmdCopyImageToBuffer(vk_command_buffer_,
                         src_texture.vk_image_handle(),
                         src_texture.current_layout_get(),
                         dst_buffer.vk_handle(),
                         regions.size(),
                         regions.data());
}
void VKCommandBuffer::copy(VKTexture &dst_texture,
                           VKBuffer &src_buffer,
                           Span<VkBufferImageCopy> regions)
{
  vkCmdCopyBufferToImage(vk_command_buffer_,
                         src_buffer.vk_handle(),
                         dst_texture.vk_image_handle(),
                         dst_texture.current_layout_get(),
                         regions.size(),
                         regions.data());
}

void VKCommandBuffer::clear(VkImage vk_image,
                            VkImageLayout vk_image_layout,
                            const VkClearColorValue &vk_clear_color,
                            Span<VkImageSubresourceRange> ranges)
{
  vkCmdClearColorImage(vk_command_buffer_,
                       vk_image,
                       vk_image_layout,
                       &vk_clear_color,
                       ranges.size(),
                       ranges.data());
}

void VKCommandBuffer::clear(Span<VkClearAttachment> attachments, Span<VkClearRect> areas)
{
  vkCmdClearAttachments(
      vk_command_buffer_, attachments.size(), attachments.data(), areas.size(), areas.data());
}

void VKCommandBuffer::pipeline_barrier(VkPipelineStageFlags source_stages,
                                       VkPipelineStageFlags destination_stages)
{
  vkCmdPipelineBarrier(vk_command_buffer_,
                       source_stages,
                       destination_stages,
                       0,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       0,
                       nullptr);
}

void VKCommandBuffer::pipeline_barrier(Span<VkImageMemoryBarrier> image_memory_barriers)
{
  vkCmdPipelineBarrier(vk_command_buffer_,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_DEPENDENCY_BY_REGION_BIT,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       image_memory_barriers.size(),
                       image_memory_barriers.data());
}

void VKCommandBuffer::dispatch(int groups_x_len, int groups_y_len, int groups_z_len)
{
  vkCmdDispatch(vk_command_buffer_, groups_x_len, groups_y_len, groups_z_len);
}

void VKCommandBuffer::submit()
{
  end_recording();
  encode_recorded_commands();
  submit_encoded_commands();
  begin_recording();
}

void VKCommandBuffer::encode_recorded_commands()
{
  /* Intentionally not implemented. For the graphics pipeline we want to extract the
   * resources and its usages so we can encode multiple commands in the same command buffer with
   * the correct synchronizations. */
}

void VKCommandBuffer::submit_encoded_commands()
{
  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &vk_command_buffer_;

  vkQueueSubmit(vk_queue_, 1, &submit_info, vk_fence_);
  submission_id_.next();
}

}  // namespace blender::gpu
