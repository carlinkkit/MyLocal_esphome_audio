#include "adf_media_player.h"
#include "esphome/core/log.h"

namespace esphome {
namespace adf_media_player {

static const char *TAG = "adf_media_player";

void ADFMediaPlayer::setup() {
  esph_log_i(TAG, "Setting up ADF Media Player");
  pipeline.setup();
}

void ADFMediaPlayer::loop() {
  pipeline.loop();
}

void ADFMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (pipeline.getState() == PipelineState::STOPPED || pipeline.getState() == PipelineState::UNINITIALIZED) {
          pipeline.start();
        } else if (pipeline.getState() == PipelineState::PAUSED) {
          pipeline.restart();
        } else if (state == media_player::MEDIA_PLAYER_STATE_PLAYING || state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
          this->play_intent_ = true;
          pipeline.stop();
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        if (pipeline.getState() == PipelineState::RUNNING) {
          pipeline.pause();
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        this->current_track_.reset();
        this->current_uri_.reset();
        pipeline.stop();
        this->http_and_decoder_.set_fixed_settings(false);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->mute_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->unmute_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (pipeline.getState() == PipelineState::STOPPED || pipeline.getState() == PipelineState::PAUSED) {
          state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        } else {
          state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        float new_volume = this->volume + 0.1f;
        if (new_volume > 1.0f)
          new_volume = 1.0f;
        set_volume_(new_volume);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        float new_volume = this->volume - 0.1f;
        if (new_volume < 0.0f)
          new_volume = 0.0f;
        set_volume_(new_volume);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_ENQUEUE:
      case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ONE:
      case media_player::MEDIA_PLAYER_COMMAND_REPEAT_OFF:
      case media_player::MEDIA_PLAYER_COMMAND_CLEAR_PLAYLIST:
        esph_log_w(TAG, "Unhandled media player command: %d", call.get_command().value());
        break;
      default:
        esph_log_w(TAG, "Unknown media player command: %d", call.get_command().value());
        break;
    }
  }
}

void ADFMediaPlayer::mute_() {
  this->is_muted_ = true;
  set_volume_(0.0f);
}

void ADFMediaPlayer::unmute_() {
  this->is_muted_ = false;
  set_volume_(this->previous_volume_);
}

void ADFMediaPlayer::set_volume_(float volume) {
  this->previous_volume_ = this->volume;
  this->volume = volume;
  esph_log_i(TAG, "Setting volume to %.2f", volume);
}

}  // namespace adf_media_player
}  // namespace esphome
