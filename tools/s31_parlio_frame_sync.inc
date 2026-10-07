bool ParlioDma::wait_for_frame(uint32_t timeout_ms) {
  if (!frame_sync_.Pending()) return true;
  if (!frame_sync_.Wait(timeout_ms)) return false;
  std::swap(front_idx_, active_idx_);
  return true;
}

bool ParlioDma::present_frame(uint32_t timeout_ms) {
  if (!dma_buffers_[1] || !transfer_started_ || !wait_for_frame(timeout_ms)) return false;
  const uint8_t value = static_cast<uint8_t>(basis_brightness_ * intensity_);
  set_brightness_oe_internal(row_buffers_[active_idx_], value);
  flush_cache_to_dma(active_idx_);
  if (!frame_sync_.Queue(tx_unit_, dma_buffers_[front_idx_], dma_buffers_[active_idx_],
                         total_buffer_bytes_ * 8, &transmit_config_)) return false;
  return wait_for_frame(timeout_ms);
}

void ParlioDma::flip_buffer() {
  if (!present_frame(250)) ESP_LOGE(TAG, "Frame not released; producer must wait before drawing");
}
