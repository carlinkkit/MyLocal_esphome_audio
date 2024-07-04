#include "adf_media_player.h"
#include "esphome/core/log.h"
#include "esphome/components/api/homeassistant_service.h"
#include "esphome/components/api/custom_api_device.h"

#include <algorithm>
#include <random>

#ifdef USE_ESP_IDF
#include <esp_http_client.h>
#include <i2s_stream.h>
#include <audio_mem.h>

namespace esphome {
namespace esp_adf {

static const char *const TAG = "adf_media_player";

void ADFMediaPlayer::setup() {
  state = media_player::MEDIA_PLAYER_STATE_OFF;
}

void ADFMediaPlayer::dump_config() {
  esph_log_config(TAG, "ESP-ADF-MediaPlayer:");
  int components = pipeline.get_number_of_elements();
  esph_log_config(TAG, "  Number of ASPComponents: %d", components);
}

void ADFMediaPlayer::publish_state() {
  esph_log_d(TAG, "MP State = %s, MP Prior State = %s", media_player_state_to_string(state), media_player_state_to_string(prior_state));
  if (state != prior_state || force_publish_) {
    switch (state) {
      case media_player::MEDIA_PLAYER_STATE_PLAYING:
        set_position_();
        break;
      default:
        set_duration_(0);
        set_position_(0);
        break;
    }
    esph_log_d(TAG, "Publish State");
      this->state_callback_.call();
      this->prior_state = this->state;
      this->force_publish_ = false;
  }
}

media_player::MediaPlayerTraits ADFMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause( true );
  traits.set_supports_next_previous_track( true );
  traits.set_supports_turn_off_on( true );
  traits.set_supports_grouping( true );
  return traits;
}

void ADFMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  esph_log_d(TAG, "control call while state: %s", media_player_state_to_string(state));

  if (group_members_.length() > 0) {
    set_mrm_(media_player::MEDIA_PLAYER_MRM_LEADER);
  }

  //Media File is sent (no command)
  if (call.get_media_url().has_value())
  {
    std::string media_url = call.get_media_url().value();
    //special cases for setting mrm commands
    if (media_url == "listen") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      mrm_listen_();
    }
    else if (media_url == "unlisten") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      mrm_unlisten_();
    }
    else if (media_url == "start") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      player_start_();
    }
    else if (media_url == "stop") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      player_stop_();
    }
    else if (media_url == "resume") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      player_stop_();
    }
    else if (media_url == "uninitialize") {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      player_uninitialize_();
    }
    else if (media_url.rfind("MRM_URL", 0) == 0) {
      set_mrm_(media_player::MEDIA_PLAYER_MRM_FOLLOWER);
      std::string mrm_url = media_url.substr(8, (media_url.length() - 8));
      this->http_and_decoder_.set_stream_uri(mrm_url);
    }
    else {
      //enqueue
      media_player::MediaPlayerEnqueue enqueue = media_player::MEDIA_PLAYER_ENQUEUE_PLAY;
      if (call.get_enqueue().has_value()) {
        enqueue = call.get_enqueue().value();
      }
      // announcing
      bool announcing = false;
      if (call.get_announcement().has_value()) {
        announcing = call.get_announcement().value();
      }
      if (announcing) {
        this->play_track_id_ = next_playlist_track_id_();
        // place announcement in the announcements_ queue
        ADFUriTrack track;
        track.uri = call.get_media_url().value();
        announcements_.push_back(track);
        //stop what is currently playing, remember adf: http_stream closes connection, so 
        //behavior is the music stops, the announcment happens and the music restarts at beginning
        //separate out PAUSE, if resume ever works in future (lots of rework).
        if (state == media_player::MEDIA_PLAYER_STATE_PLAYING || state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
          this->play_intent_ = true;
          stop_();
          return;
        } 
        else if (state != media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
          start_();
        }
      }
      //normal media, use enqueue value to determine what to do
      else {
        if (enqueue == media_player::MEDIA_PLAYER_ENQUEUE_REPLACE || enqueue == media_player::MEDIA_PLAYER_ENQUEUE_PLAY) {
          this->play_track_id_ = -1;
          if (enqueue == media_player::MEDIA_PLAYER_ENQUEUE_REPLACE) {
            clean_playlist_();
          }
          playlist_add_(call.get_media_url().value(), true);
          set_playlist_track_(playlist_[0]);
          this->play_intent_ = true;
          if (state == media_player::MEDIA_PLAYER_STATE_PLAYING || state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
            this->play_track_id_ = 0;
            stop_();
            return;
          } else {
            start_();
          }
        }
        else if (enqueue == media_player::MEDIA_PLAYER_ENQUEUE_ADD) {
          playlist_add_(call.get_media_url().value(), true);
        }
        else if (enqueue == media_player::MEDIA_PLAYER_ENQUEUE_NEXT) {
          playlist_add_(call.get_media_url().value(), false);
        }
      }
    } 
  }
  // Volume value is sent (no command)
  if (call.get_volume().has_value()) {
    set_volume_(call.get_volume().value());
  }
  //Command
  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        this->play_intent_ = true;
        this->play_track_id_ = -1;
        if (state == media_player::MEDIA_PLAYER_STATE_PLAYING) {
          stop_();
        }
        // pausing doesn't work for adf: http_stream, so don't expect this to ever be
        // happening.
        if (state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
          resume_();
        }
        if (state == media_player::MEDIA_PLAYER_STATE_OFF 
        || state == media_player::MEDIA_PLAYER_STATE_ON 
        || state == media_player::MEDIA_PLAYER_STATE_NONE
        || state == media_player::MEDIA_PLAYER_STATE_IDLE) {
        
          int id = next_playlist_track_id_();
          if (id > -1) {
            set_playlist_track_(playlist_[id]);
          }
          start_();
        }
        break;
      // actually just stop and when "resume" happens restart at beginning
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        if (state == media_player::MEDIA_PLAYER_STATE_PLAYING) {
          this->play_track_id_ = next_playlist_track_id_();
          play_intent_ = false;
        }
        stop_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        play_intent_ = false;
        clean_playlist_();
        stop_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->mute_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->unmute_();
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
      case media_player::MEDIA_PLAYER_COMMAND_NEXT_TRACK: {
        if ( this->playlist_.size() > 0 ) {
          this->play_intent_ = true;
          this->play_track_id_ = -1;
          stop_();
        }
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_PREVIOUS_TRACK: {
        if ( this->playlist_.size() > 0 ) {
          this->play_intent_ = true;
          this->play_track_id_ = previous_playlist_track_id_();
          stop_();
        }
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE: {
        if (state == media_player::MEDIA_PLAYER_STATE_OFF) {
          state = media_player::MEDIA_PLAYER_STATE_ON;
          publish_state();
          mrm_turn_on_();
        }
        else {
          if (state == media_player::MEDIA_PLAYER_STATE_PLAYING 
          || state == media_player::MEDIA_PLAYER_STATE_PAUSED 
          || state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING ) {
            turning_off_ = true;
            this->play_intent_ = false;
            stop_();
          }
          else {
            state = media_player::MEDIA_PLAYER_STATE_OFF;
            publish_state();
            mrm_turn_on_();
          }
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TURN_ON: {
        if (state == media_player::MEDIA_PLAYER_STATE_OFF) {
            state = media_player::MEDIA_PLAYER_STATE_ON;
            publish_state();
            mrm_turn_on_();
        }
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_TURN_OFF: {
        if (state != media_player::MEDIA_PLAYER_STATE_OFF) {
          if (state == media_player::MEDIA_PLAYER_STATE_PLAYING 
          || state == media_player::MEDIA_PLAYER_STATE_PAUSED 
          || state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING ) {
            turning_off_ = true;
            this->play_intent_ = false;
            stop_();
          }
          else {
            state = media_player::MEDIA_PLAYER_STATE_OFF;
            publish_state();
            mrm_turn_off_();
          }
        }
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_CLEAR_PLAYLIST: {
        clean_playlist_();
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_SHUFFLE: {
        set_shuffle_(true);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_UNSHUFFLE: {
        set_shuffle_(false);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_REPEAT_OFF: {
        set_repeat_(media_player::MEDIA_PLAYER_REPEAT_OFF);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ONE: {
        set_repeat_(media_player::MEDIA_PLAYER_REPEAT_ONE);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ALL: {
        set_repeat_(media_player::MEDIA_PLAYER_REPEAT_ALL);
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_JOIN: {
        if (call.get_group_members().has_value()) {
          group_members_ = call.get_group_members().value();
          set_mrm_(media_player::MEDIA_PLAYER_MRM_LEADER);
          mrm_listen_();
        }
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_UNJOIN: {
        set_mrm_(media_player::MEDIA_PLAYER_MRM_LEADER);
        mrm_unlisten_();
        group_members_ = "";
        set_mrm_(media_player::MEDIA_PLAYER_MRM_OFF);
        break;
      }
      default:
        break;
      }
    }
  }
}

void ADFMediaPlayer::set_position_() {
  if (filesize_ > 0) {
    audio_element_info_t info{};
    audio_element_getinfo(http_and_decoder_.get_decoder(), &info);
    int64_t position_byte = info.byte_pos;
    if (position_byte > 0) {
      position_ = round(((position_byte * 1.0) / (filesize_ * 1.0)) * (duration_ * 1.0));
      esph_log_v(TAG, "set_position: %lld %lld %d %d", position_byte, filesize_, duration_, position_);
    }
  }
}

void ADFMediaPlayer::on_pipeline_state_change(PipelineState state) {
  esph_log_i(TAG, "got new pipeline state: %s", pipeline_state_to_string(state));
  switch (state) {
    case PipelineState::PREPARING:
    case PipelineState::STARTING:
    case PipelineState::RESUMING:
    case PipelineState::RUNNING:
      this->set_volume_( this->volume, false);
      if (is_announcement_()) {
        this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
      }
      else {
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
      }
      publish_state();
      break;
    case PipelineState::STOPPING:
      mrm_stop_();
      set_artist_("");
      set_album_("");
      set_title_("");
      set_duration_(0);
      set_filesize_(0);
      set_position_(0);
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      publish_state();
      break;
    case PipelineState::STOPPED:
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      publish_state();

      if (this->turning_off_) {
            player_uninitialize_();
            mrm_uninitialize_();
      }
      else {
        if (this->play_intent_) {
          if (!play_next_track_on_announcements_()) {
            play_next_track_on_playlist_(this->play_track_id_);
            this->play_track_id_ = -1;
          }
        }
        if (this->play_intent_) {
          start_();
        }
        else {
          // clean up completing when playlist_ and announcements_ are empty
          if (mrm_ != media_player::MEDIA_PLAYER_MRM_FOLLOWER) {
            player_uninitialize_();
            mrm_uninitialize_();
          }
        }
      }
      break;
    case PipelineState::DESTROYING:
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      publish_state();
      break;
    case PipelineState::UNINITIALIZED:
      if (this->turning_off_) {
        this->state = media_player::MEDIA_PLAYER_STATE_OFF;
        publish_state();
        mrm_turn_off_();
        turning_off_ = false;
      }
      else {
        this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
        publish_state();
      }
      break;
    case PipelineState::PAUSING:
    case PipelineState::PAUSED:
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      publish_state();
      break;
    default:
      break;
  }
}

void ADFMediaPlayer::start_() 
{
  esph_log_d(TAG,"start_()");
  mrm_start_();
  player_start_();
}

void ADFMediaPlayer::player_start_() {
  
  if (state == media_player::MEDIA_PLAYER_STATE_OFF 
  || state == media_player::MEDIA_PLAYER_STATE_ON 
  || state == media_player::MEDIA_PLAYER_STATE_NONE
  || state == media_player::MEDIA_PLAYER_STATE_IDLE) {
    esph_log_d(TAG,"player_start_()");
    if (state == media_player::MEDIA_PLAYER_STATE_OFF) {
      state = media_player::MEDIA_PLAYER_STATE_ON;
      publish_state();
    }
    //will force destroy when playlist and announcements are finished.
    //this ignores whatever setting is done in yaml.
    pipeline.set_destroy_on_stop(false);
    pipeline.start();
  }
}

void ADFMediaPlayer::stop_() {
  esph_log_d(TAG,"stop_()");
  player_stop_();
  if (turning_off_) {
    mrm_turn_off_();
  }
  else {
    mrm_stop_();
  }
}

void ADFMediaPlayer::player_stop_() {
  if (state == media_player::MEDIA_PLAYER_STATE_PLAYING || state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
    esph_log_d(TAG,"player_stop_()");
    pipeline.stop();
  }
}

void ADFMediaPlayer::resume_()
{
  esph_log_d(TAG,"resume_()");
  mrm_resume_();
  player_resume_();
}

void ADFMediaPlayer::player_resume_()
{
  if (state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
    esph_log_d(TAG,"player_resume_()");
    pipeline.resume();
  }
}

void ADFMediaPlayer::player_uninitialize_() {
  if (pipeline.getState() != PipelineState::DESTROYING && pipeline.getState() != PipelineState::UNINITIALIZED)
  pipeline.reset(true);
}

bool ADFMediaPlayer::is_announcement_() {
  return this->http_and_decoder_.is_announcement();
}

void ADFMediaPlayer::set_volume_(float volume, bool publish) {
  AudioPipelineSettingsRequest request;
  request.target_volume = volume;
  if (pipeline.request_settings(request)) {
    this->volume = volume;
    if (publish) {
      force_publish_ = true;
      publish_state();
      mrm_volume_();
    }
  }
}

void ADFMediaPlayer::mute_() {
  AudioPipelineSettingsRequest request;
  request.mute = 1;
  if (pipeline.request_settings(request)) {
    muted_ = true;
    force_publish_ = true;
    publish_state();
    mrm_mute_();
  }
}

void ADFMediaPlayer::unmute_() {
  AudioPipelineSettingsRequest request;
  request.target_volume = volume;
  request.mute = 0;
  if (pipeline.request_settings(request)) {
    muted_ = false;
    force_publish_ = true;
    publish_state();
    mrm_unmute_();
  }
}

// from Component
void ADFMediaPlayer::loop() { 
    mrm_process_recv_actions_();
    mrm_process_send_actions_();
  ADFPipelineController::loop();
}

void ADFMediaPlayer::mrm_process_send_actions_() {
  if (mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    if (pipeline.getState() == PipelineState::RUNNING
        && duration() > 0
        && ((udpMRM_.get_timestamp() - position_timestamp_) > 10000000L)) {
      mrm_send_position_();
    }
  }
  if (mrm_ == media_player::MEDIA_PLAYER_MRM_FOLLOWER) {
    if (pipeline.getState() == PipelineState::UNINITIALIZED) {
      udpMRM_.send_ping();
    }
  }
}

void ADFMediaPlayer::mrm_process_recv_actions_() {  
  if (this->udpMRM_.recv_actions.size() > 0) {
    std::string action = this->udpMRM_.recv_actions.front().type;

    if (action == "sync_position" && mrm_ == media_player::MEDIA_PLAYER_MRM_FOLLOWER) {
      int64_t timestamp = this->udpMRM_.recv_actions.front().timestamp;
      std::string position_str = this->udpMRM_.recv_actions.front().data;
      int64_t position = strtoll(position_str.c_str(), NULL, 10);
      this->mrm_sync_position_(timestamp, position);
    }
    this->udpMRM_.recv_actions.pop();
  }
}

void ADFMediaPlayer::mrm_sync_position_(int64_t timestamp, int64_t position) {
  if (mrm_ == media_player::MEDIA_PLAYER_MRM_FOLLOWER && pipeline.getState() == PipelineState::RUNNING) {
    audio_element_info_t info{};
    audio_element_getinfo(pipeline.get_last_audio_element(), &info);
    int64_t local_timestamp = udpMRM_.get_timestamp();
    int64_t local_position = info.byte_pos;
    if (local_position > 100 * 1024) {
      int32_t bps = (int32_t)(info.sample_rates * info.channels * info.bits / 8);
      int64_t adjusted_position = (((local_timestamp - (timestamp - udpMRM_.offset)) / 1000000.0) * bps) + position;
      int32_t delay_size = (int32_t)(adjusted_position - local_position);
      esph_log_d(TAG,"sync_position: follower timestamp: %lld, leader: %lld, follower: %lld, diff: %d", local_timestamp, adjusted_position, local_position, delay_size);
      if (abs(delay_size) > 10 * 1024) {
        ringbuf_handle_t rb = audio_element_get_input_ringbuf(pipeline.get_last_audio_element());
        audio_element_pause(http_and_decoder_.get_decoder());
        if (delay_size > (.8 * rb_bytes_filled(rb))) {
          delay_size = round(rb_bytes_filled(rb) * .8);
        }
        else if (delay_size < 0 && abs(delay_size) > (.8 * rb_bytes_available(rb))) {
          if (rb_bytes_available(rb) < 1) {
            for(int i = 1; i <= 100000; ++i) {
              if (rb_bytes_available(rb) > (.5 * rb_get_size(rb) )) {
                break;
              }
            }
          }
          if (delay_size > round(rb_bytes_available(rb) * -.8)) {
            delay_size = round(rb_bytes_available(rb) * -.8);
          }
        }
        if (delay_size != 0) {
          audio_element_update_byte_pos(pipeline.get_last_audio_element(), (delay_size));
        }
        audio_element_resume(http_and_decoder_.get_decoder(), 0, 2000 / portTICK_RATE_MS);
        esph_log_d(TAG,"sync_position done, delay_size: %d", delay_size);
      }
    }
  }
}

void ADFMediaPlayer::mrm_command_(const std::string command) {
  
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {

    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.play_media";
    
    //TBD - this requires a change in core esphome media-player
    esphome::api::HomeassistantServiceMap kv2;
    kv2.key = "media_content_id";
    kv2.value = command;
    esphome::api::HomeassistantServiceMap kv3;
    kv3.key = "media_content_type";
    kv3.value = "music";

    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s %s", resp.service.c_str(), token, command.c_str());
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      resp.data.push_back(kv2);
      resp.data.push_back(kv3);
      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::mrm_listen_() {
  if (mrm_ != media_player::MEDIA_PLAYER_MRM_OFF) {
    esph_log_d(TAG, "udpMRM_ listen");
    this->udpMRM_.listen(mrm_);
  }
}

void ADFMediaPlayer::mrm_unlisten_() {
  if (mrm_ != media_player::MEDIA_PLAYER_MRM_OFF) {
    esph_log_d(TAG, "udpMRM_ unlisten");
    this->udpMRM_.unlisten();
  }
}

void ADFMediaPlayer::mrm_set_stream_uri_(const std::string url) {
  mrm_command_("MRM_URL:" + url);
}

void ADFMediaPlayer::mrm_start_() {
  mrm_listen_();
  mrm_command_("start");
}

void ADFMediaPlayer::mrm_stop_() {
  mrm_command_("stop");
}

void ADFMediaPlayer::mrm_resume_() {
  mrm_listen_();
  mrm_command_("resume");
}

void ADFMediaPlayer::mrm_uninitialize_() {
  mrm_command_("uninitialize");
}

void ADFMediaPlayer::mrm_send_position_() {
  if (mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER && pipeline.getState() == PipelineState::RUNNING) {
    audio_element_info_t info{};
    audio_element_getinfo(pipeline.get_last_audio_element(), &info);
    position_timestamp_ = udpMRM_.get_timestamp();
    int64_t position_byte = info.byte_pos;
    if (position_byte > 100 * 1024) {
      esph_log_v(TAG, "udpMRM_ send position");
      this->udpMRM_.send_position(position_timestamp_, position_byte);
    }
  }
}

void ADFMediaPlayer::mrm_turn_on_() {
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.turn_on";
    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s turn_on", resp.service.c_str(), token);
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::mrm_turn_off_() {
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.turn_off";
    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s turn_off", resp.service.c_str(), token);
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::mrm_volume_() {
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.volume_set";
    esphome::api::HomeassistantServiceMap kv2;
    kv2.key = "volume_level";
    kv2.value = to_string(volume);

    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s volume", resp.service.c_str(), token);
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      resp.data.push_back(kv2);

      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::mrm_mute_() {
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.volume_mute";
    esphome::api::HomeassistantServiceMap kv2;
    kv2.key = "is_volume_muted";
    kv2.value = "true";

    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s mute", resp.service.c_str(), token);
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      resp.data.push_back(kv2);

      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::mrm_unmute_() {
  if (group_members_.length() > 0 && mrm_ == media_player::MEDIA_PLAYER_MRM_LEADER) {
    std::string group_members = group_members_ + ",";
    char *token = strtok(const_cast<char*>(group_members.c_str()), ",");
    esphome::api::HomeassistantServiceResponse resp;
    resp.service = "media_player.volume_mute";
    esphome::api::HomeassistantServiceMap kv2;
    kv2.key = "is_volume_muted";
    kv2.value = "false";

    while (token != nullptr)
    {
      esph_log_d(TAG, "%s on %s unmute", resp.service.c_str(), token);
      esphome::api::HomeassistantServiceMap kv1;
      kv1.key = "entity_id";
      kv1.value = token;
      resp.data.push_back(kv1);
      resp.data.push_back(kv2);

      esphome::api::global_api_server->send_homeassistant_service_call(resp);
      token = strtok(nullptr, ",");
    }
  }
}

void ADFMediaPlayer::set_mrm_(media_player::MediaPlayerMRM mrm) {
  this->mrm_ = mrm;
}

void ADFMediaPlayer::set_repeat_(media_player::MediaPlayerRepeatMode repeat) {
  this->repeat_ = repeat;
  force_publish_ = true;
  publish_state();
}

void ADFMediaPlayer::set_shuffle_(bool shuffle) {
  unsigned int vid = this->playlist_.size();
  
  if (vid > 0) {
    if (shuffle) {
      auto rng = std::default_random_engine {};
      std::shuffle(std::begin(playlist_), std::end(playlist_), rng);
    }
    else {
      sort(playlist_.begin(), playlist_.end());
    }
    for(unsigned int i = 0; i < vid; i++)
    {
      this->playlist_[i].is_played = false;
    }
    
    this->shuffle_ = shuffle;
    force_publish_ = true;
    publish_state();
    this->play_intent_ = true;
    this->play_track_id_ = 0;
    stop_();
  }
}

void ADFMediaPlayer::playlist_add_(const std::string& uri, bool toBack) {
  
  esph_log_v(TAG, "playlist_add_: %s", uri.c_str());

  unsigned int vid = this->playlist_.size();

  if (uri.find("m3u") != std::string::npos) {
    int pl = parse_m3u_into_playlist_(uri.c_str(), toBack);
  }
  else {
    if (!toBack) {
      update_playlist_order(1);
      vid = 0;
    }
    ADFPlaylistTrack track;
    track.uri = uri;
    track.order = 0;
    playlist_.push_back(track);
  }
  
  esph_log_v(TAG, "Number of playlist tracks = %d", playlist_.size());
}

void ADFMediaPlayer::set_playlist_track_(ADFPlaylistTrack track) {
  esph_log_d(TAG, "set_playlist_track: %s: %s: %s duration: %d filesize: %lld",
     track.artist.c_str(), track.album.c_str(), track.title.c_str(), track.duration, track.filesize);
  esph_log_v(TAG, "uri: %s", trak.uri);
  set_artist_(track.artist);
  set_album_(track.album);
  if (track.title == "") {
    set_title_(track.uri);
  }
  else {
    set_title_(track.title);
  }
  set_duration_(track.duration);
  set_filesize_(track.filesize);

  http_and_decoder_.set_stream_uri(track.uri);
  mrm_set_stream_uri_(track.uri);
}

void ADFMediaPlayer::update_playlist_order(unsigned int start_order) {
  unsigned int vid = this->playlist_.size();
  if (vid > 0) {
    sort(playlist_.begin(), playlist_.end());
    for(unsigned int i = 0; i < vid; i++)
    {
      this->playlist_[i].order = i + start_order;
    }
  }
}

void ADFMediaPlayer::play_next_track_on_playlist_(int track_id) {

  unsigned int vid = this->playlist_.size();
  if (playlist_.size() > 0) {
    if (repeat_ != media_player::MEDIA_PLAYER_REPEAT_ONE) {
      set_playlist_track_as_played_(track_id);
    }
    int id = next_playlist_track_id_();
    if (id > -1) {
      set_playlist_track_(playlist_[id]);
    }
    else {
      if (repeat_ == media_player::MEDIA_PLAYER_REPEAT_ALL) {
        for(unsigned int i = 0; i < vid; i++)
        {
          this->playlist_[i].is_played = false;
        }
        set_playlist_track_(playlist_[0]);
      }
      else {
        clean_playlist_();
        this->play_intent_ = false;
      }
    }
  }
}

void ADFMediaPlayer::clean_playlist_()
{
  unsigned int vid = this->playlist_.size();
  if ( this->playlist_.size() > 0 ) {
    /*
    for(unsigned int i = 0; i < vid; i++)
    {
      delete this->&playlist_[i];
    }
    */
    this->playlist_.clear();
    play_track_id_ = -1;
  }
}

int ADFMediaPlayer::next_playlist_track_id_()
{
  unsigned int vid = this->playlist_.size();
  if ( vid > 0 ) {
    for(unsigned int i = 0; i < vid; i++)
    {
      bool ip = this->playlist_[i].is_played;
      if (!ip) {
        return i;
      }
    }
  }
  return -1;
}

int ADFMediaPlayer::previous_playlist_track_id_()
{
  unsigned int vid = this->playlist_.size();
  if ( vid > 0 ) {
    for(unsigned int i = 0; i < vid; i++)
    {
      bool ip = this->playlist_[i].is_played;
      if (!ip) {
        int j = i-1;
        if (j < 0) j = 0;
        this->playlist_[j].is_played = false;
        return j;
      }
    }
  }
  return -1;
}

void ADFMediaPlayer::set_playlist_track_as_played_(int track_id)
{
  if ( this->playlist_.size() > 0 ) {
      unsigned int vid = this->playlist_.size();
    if (track_id < 0) {
      for(unsigned int i = 0; i < vid; i++)
      {
        bool ip = this->playlist_[i].is_played;
        if (!ip) {
          this->playlist_[i].is_played = true;
          break;
        }
      }
    }
    else {
      for(unsigned int i = 0; i < vid; i++)
      {
        if (i < track_id) {
          this->playlist_[i].is_played = true;
        }
        else {
          this->playlist_[i].is_played = false;
        }
      }
    }
  }
}

int ADFMediaPlayer::parse_m3u_into_playlist_(const char *url, bool toBack)
{
  
    unsigned int vid = this->playlist_.size();
    esp_http_client_config_t config = {
        .url = url
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char *response;
    esp_err_t err = esp_http_client_open(client,0);
    int rc = 0;
    if (err == ESP_OK) {
      //int cl = esp_http_client_get_content_length(client);
      int cl =  esp_http_client_fetch_headers(client);
      esph_log_v(TAG, "HTTP Status = %d, content_length = %d", esp_http_client_get_status_code(client), cl);
      response = (char *)malloc(cl + 1);
      if (response == NULL) {
        esph_log_e(TAG, "Cannot malloc http receive buffer");
        rc = -1;
      }
      else {
        int rl = esp_http_client_read(client, response, cl); 
        if (rl < 0) {
          esph_log_e(TAG, "HTTP request failed: %s, ", esp_err_to_name(err));
          free(response);
          rc = -1;
        }
        else {
          response[cl] = '\0';
          esph_log_v(TAG, "Response: %s", response);
          size_t start = 0;
          size_t end;
          char *ptr;
          char *buffer = response;
          bool keeplooping = true;
          std::string artist = "";
          std::string album = "";
          std::string title = "";
          int duration = 0;
          int64_t filesize = 0;
          if (toBack) {
            update_playlist_order(1000);
            vid = 0;
          }
          while (keeplooping) {
            ptr = strchr(buffer , '\n' );
            if (ptr != NULL)
            {
              end = ptr - response;
            }
            else {
              end = cl;
              keeplooping = false;
            }
            size_t lngth = (end - start);
            //Finding a carriage return, assume its a Windows file and this line ends with "\r\n"
            if (strchr(buffer,'\r') != NULL) {
              lngth--;
            }
            if (lngth > 0) {
              char *cLine;
              cLine = (char *)malloc(lngth + 1);
              if (cLine == NULL) {
                esph_log_e(TAG, "Cannot malloc cLine");
                free(response);
                rc = -1;
                break;
              }
              sprintf (cLine,"%.*s", lngth, buffer);
              cLine[lngth] = '\0';
              esph_log_v(TAG, "%s", cLine);
              if (strstr(cLine,"#EXTART:") != NULL) {
                artist = cLine + 8;
              }
              else if (strstr(cLine,"#EXTALB:") != NULL) {
                if (strchr(cLine,'-') != NULL) {
                  album = strchr(cLine,'-') + 1;
                }
                else {
                  album = cLine + 8;
                }
              }
              if (strstr(cLine,"#EXTBYT:") != NULL) {
                  float filesize_f = strtof(cLine + 8, NULL);
                  filesize = round(filesize_f);
              }
              else if (strstr(cLine,"#EXTINF:") != NULL) {
                char *comma_ptr;
                comma_ptr = strchr(cLine,',');
                if (comma_ptr != NULL) {
                  title = strchr(cLine,',') + 1;
                  std::string inf_line = cLine + 8;
                  size_t comma_sz = comma_ptr - (cLine + 8);
                  std::string duration_str = inf_line.substr(0, comma_sz);
                  float duration_f = strtof(duration_str.c_str(), NULL);
                  duration = round(duration_f);
                }
              }
              else if (strchr(cLine,'#') == NULL) {
                esph_log_v(TAG, "Create Playlist Track %s: %s: %s duration: %d filesize: %lld",
                  artist.c_str(),album.c_str(),title.c_str(),duration,filesize);
                ADFPlaylistTrack track;
                track.uri = cLine;
                track.order = vid;
                track.artist = artist;
                track.album = album;
                track.title = title;
                track.duration = duration;
                track.filesize = filesize;
                playlist_.push_back(track);
                vid++;
              }
              free(cLine);
            }
            start = end + 1;
            buffer = ptr + 1;
          }
          free(response);
        }
      }
    } else {
        esph_log_e(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        rc = -1;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (toBack) {
        update_playlist_order(0);
    }
    if (shuffle_) {
      auto rng = std::default_random_engine {};
      std::shuffle(std::begin(playlist_), std::end(playlist_), rng);
      unsigned int vid = this->playlist_.size();
      for(unsigned int i = 0; i < vid; i++)
      {
        esph_log_v(TAG, "Playlist: %s", playlist_[i].uri.c_str());
        this->playlist_[i].is_played = false;
      }
    }
    return rc;
}

void ADFMediaPlayer::clean_announcements_()
{
  unsigned int vid = this->announcements_.size();
  if ( vid > 0 ) {
    /*
    for(unsigned int i = 0; i < vid; i++)
    {
      delete this->announcements_[i];
    }
    */
    this->announcements_.clear();
  }
}

bool ADFMediaPlayer::play_next_track_on_announcements_() {
  bool retBool = false;
  unsigned int vid = this->announcements_.size();
  if (vid > 0) {
    for(unsigned int i = 0; i < vid; i++) {
      bool ip = this->announcements_[i].is_played;
      if (!ip) {
        http_and_decoder_.set_stream_uri(announcements_[i].uri, true);
        announcements_[i].is_played = true;
        retBool = true;
        mrm_set_stream_uri_(announcements_[i].uri);
      }
    }
    if (!retBool) {
      clean_announcements_();
    }
  }
  return retBool;
}

}  // namespace esp_adf
}  // namespace esphome
#endif
