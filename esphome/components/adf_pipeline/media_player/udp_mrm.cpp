#include "udp_mrm.h"

#include <string.h>
#include <sstream>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esphome/core/log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <cJSON.h>
#include <algorithm>

#include "esphome/components/network/ip_address.h"
#include "esphome/components/network/util.h"

#ifdef USE_ESP_IDF

namespace esphome {
namespace esp_adf {

static const char *const TAG = "udp_mcast";

// Refer to "esp-idf/examples/protocols/sockets/udp_multicast/main/udp_multicast_example_main.c"
// as basis for the multicast coding, only coded for IPV4

static void mrm_task(void *pvParameters)
{
  UdpMRM& self = *((UdpMRM*)pvParameters);
  char recvbuf[self.multicast_buffer_size] = { 0 };
  char addr_name[32] = { 0 };

  // outer loop, used to rebuild socket on failures.
  int socket_loop_count = 0;
  while (true) {

    // create socket
    int sock = self.create_multicast_ipv4_socket();
    if (sock < 0) {
      esph_log_e(TAG, "Failed to create IPv4 multicast socket");
      vTaskDelay(5 / portTICK_PERIOD_MS);
      // try again
      if (socket_loop_count < 1000) {
        socket_loop_count++;
        continue;
      }
      else {
        esph_log_e(TAG, "1000 attempts exceeded, stopping");
        self.multicast_running = false;
        self.stop_multicast = false;
        vTaskDelete(self.xhandle);
        break;
      }
    }

    // Loop first look for UDP datagrams and convert to recv_actions 
    // and then send UDP datagrams if send_actions available.
    // all failures are handled as breaks from this loop, so while 
    // never processes a retval 0 or less.
    int retval = 1;
    std::string message_prior = "";
    while (retval > 0) {

      // break if stop requested
      if (self.stop_multicast) {
        esph_log_d(TAG, "Stopping multicast");
        break;
      }

      struct timeval tv = {
        .tv_sec = 2,
        .tv_usec = 0,
      };
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(sock, &rfds);

      int s = select(sock + 1, &rfds, NULL, NULL, &tv);

      // socket failure
      if (s < 0) {
        esph_log_e(TAG, "Select failed: errno %d", errno);
        retval = s;
        break;
      }

      // get actions from multicast
      if (s > 0) {
        if (FD_ISSET(sock, &rfds)) {
          // Incoming datagram received
          struct sockaddr_storage raddr; // Large enough for both IPv4 or IPv6
          socklen_t socklen = sizeof(raddr);
          int len = recvfrom(sock, recvbuf, sizeof(recvbuf)-1, 0, (struct sockaddr *)&raddr, &socklen);
          if (len < 0) {
            esph_log_e(TAG, "multicast recvfrom failed: errno %d", errno);
            retval = len;
            break;
          }

          // Get the sender's address as a string
          if (raddr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&raddr)->sin_addr, addr_name, sizeof(addr_name)-1);
          }
          recvbuf[len] = 0; // Null-terminate whatever we received and treat like a string...
          std::string message = recvbuf;
          std::string address = addr_name;
          if (message != message_prior) {
            self.process_multicast_message(message, address);
            message_prior = message;
          }
        }
      }
      else {
        // send actions to multicast
        if (self.send_actions.size() > 0) {
          std::string message = self.send_actions.front().to_string();
          self.send_actions.pop();

          int len = message.length();
          if (len > self.multicast_buffer_size) {
            esph_log_e(TAG, "%d is larger than will be able to be received: %d", len, self.multicast_buffer_size);
            retval = -1;
            break;
          }

          struct addrinfo hints = {
            .ai_flags = AI_PASSIVE,
            .ai_socktype = SOCK_DGRAM,
          };
          struct addrinfo *res;
          hints.ai_family = AF_INET; // For an IPv4 socket

          retval = getaddrinfo(self.multicast_ipv4_addr.c_str(), NULL, &hints, &res);
          if (retval < 0) {
            esph_log_e(TAG, "getaddrinfo() failed for IPV4 destination address. error: %d", retval);
            break;
          }
          if (res == 0) {
            esph_log_e(TAG, "getaddrinfo() did not return any addresses");
            retval = -1;
            break;
          }
          ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(self.udp_port);
          inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, addr_name, sizeof(addr_name)-1);
          retval = sendto(sock, message.c_str(), len, 0, res->ai_addr, res->ai_addrlen);
          freeaddrinfo(res);
          if (retval < 0) {
            esph_log_e(TAG, "IPV4 sendto failed. errno: %d", errno);
            break;
          }
        }
      }
    }

    esph_log_d(TAG, "Shutdown and close socket");
    shutdown(sock, 0);
    close(sock);
    if (self.stop_multicast) {
      self.multicast_running = false;
      self.stop_multicast = false;
      vTaskDelete(self.xhandle);
      break;
    }
    else {
      esph_log_d(TAG, "Restarting ...");
    }
  }
}

/* below are class methods */

std::string UdpMRMAction::to_string() {

  std::string message = "{\"action\":\"" + this->type + "\"";
    std::ostringstream otimestamp;
    otimestamp << this->get_timestamp();
    // send time as a string so that cJSON can parse
    message += ",\"timestamp\":\"" + otimestamp.str()+"\"";

  if (this->type == "ping_response") {
    std::ostringstream otimestampr;
    otimestampr << this->timestamp;
    // send time as a string so that cJSON can parse
    message += ",\"received_time\":\"" + otimestampr.str()+"\"";
    message += ",\"ping_time\":\"" + data1 +"\"";
    message += ",\"sender\":\"" + data +"\"";
  }
  else if (this->type == "sync_position") {
    std::ostringstream otimestampt;
    otimestampt << this->timestamp;
    // send time as a string so that cJSON can parse
    message += ",\"position_timestamp\":\"" + otimestampt.str()+"\"";
    message += ",\"position\":\"" + this->data + "\"";
  }
  message += "}";
  esph_log_d(TAG, "Send: %s", message.c_str());
  return message;
}

void UdpMRM::listen(media_player::MediaPlayerMRM mrm) {
  mrm_ = mrm;
  ping_send_timestamp = get_timestamp() - 590000000L;
  stop_multicast = false;
  ping_startup_cnt = 0;
  ping_startup_timestamp = 0;
  if (!multicast_running) {
    esph_log_d(TAG, "Start multicast");
    esph_log_d(TAG, "Create Task 'mrm_task'");
    xTaskCreatePinnedToCore(&mrm_task, "mrm_task", 4096, this, 5, &xhandle, 1);
    multicast_running = true;
  }
  else {
    esph_log_d(TAG, "Task 'mrm_task' already exists");
  }
}

void UdpMRM::unlisten() {
  if (!multicast_running) {
    esph_log_d(TAG, "Stop multicast");
    stop_multicast = true;
  }
}

void UdpMRM::send_ping() {
  if (get_mrm() == media_player::MEDIA_PLAYER_MRM_FOLLOWER && ((ping_startup_cnt < 11 && (get_timestamp() - ping_startup_timestamp) > 3000000L) || (get_timestamp() - ping_send_timestamp) > 600000000L) ) {
    if (ping_startup_cnt < 11 && (get_timestamp() - ping_startup_timestamp) > 3000000L) {
      ping_startup_timestamp = get_timestamp();
      ping_startup_cnt++;
    }
    ping_send_timestamp = get_timestamp();
    UdpMRMAction action;
    action.type = "ping";
    send_actions.push(action);
  }
}

void UdpMRM::send_position(int64_t timestamp, int64_t position) {
  std::ostringstream oss;
  oss << position;
  UdpMRMAction action;
  action.type = "sync_position";
  action.timestamp = timestamp;
  action.data = oss.str();
  send_actions.push(action);

}

void UdpMRM::process_multicast_message(std::string &message, std::string &sender) {
  if (message.length() > 0) {
    esph_log_d(TAG, "Received: %s from %s", message.c_str(), sender.c_str());
    
    cJSON *root = cJSON_Parse(message.c_str());
    std::string recv_action = cJSON_GetObjectItem(root,"action")->valuestring;

    // Only Leader can send a ping response to provide master clock.
    if (recv_action == "ping") {
        if (get_mrm() == media_player::MEDIA_PLAYER_MRM_LEADER) {
        UdpMRMAction action;
        action.type = "ping_response";
        action.data = sender;
        action.timestamp = get_timestamp();
        std::string ping_timestamp_str = cJSON_GetObjectItem(root,"timestamp")->valuestring;
        action.data1 = ping_timestamp_str;
        send_actions.push(action);
      }
    }
    // Only Followers respond to below actions
    else if (recv_action == "ping_response") {
      if (get_mrm() == media_player::MEDIA_PLAYER_MRM_FOLLOWER) {
        int64_t follower_timestamp = get_timestamp();
        std::string originator = cJSON_GetObjectItem(root,"sender")->valuestring;
        if (originator == this_addr_) {
          std::string leader_timestampr_str = cJSON_GetObjectItem(root,"received_time")->valuestring;
          std::string leader_timestamps_str = cJSON_GetObjectItem(root,"timestamp")->valuestring;
          std::string ping_timestamp_str = cJSON_GetObjectItem(root,"ping_time")->valuestring;
          int64_t leader_timestampr = strtoll(leader_timestampr_str.c_str(), NULL, 10);
          int64_t leader_timestamps = strtoll(leader_timestamps_str.c_str(), NULL, 10);
          int64_t ping_timestamp    = strtoll(ping_timestamp_str.c_str(), NULL, 10);
          int64_t duration = ((follower_timestamp - ping_timestamp) - (leader_timestamps - leader_timestampr));
          //positive means that leader is ahead of follower for the same time.
          //leader time = follower time + offset

          //offset is a rolling average of last 10.
          int64_t calc_offset = round((leader_timestamps + (.5 * duration)) - follower_timestamp);
          
          esph_log_d(TAG,"%lld, %lld, Offset calculated from ping (microseconds): %lld", follower_timestamp, duration, calc_offset);

          if (offsets.size() > 9) {
            offsets.pop();
          }
          offsets.push(calc_offset);

          int64_t sum_offset = 0;
          std::queue<int64_t>tmpq(offsets);
          while (!tmpq.empty()) {
            sum_offset = std::move(sum_offset) + tmpq.front();
            tmpq.pop();
          }
          offset = round((1.0 * sum_offset) / (1.0 * offsets.size()));

          esph_log_d(TAG,"average offset (microseconds): %lld", offset);
        }
      }
    }
    else if (recv_action == "sync_position") {
      if (get_mrm() == media_player::MEDIA_PLAYER_MRM_FOLLOWER) {
        esph_log_d(TAG, "Processing received sync_position");
        // process this action to speed up or slow down followers output to sync with leader
        std::string timestamp_str = cJSON_GetObjectItem(root,"position_timestamp")->valuestring;
        int64_t timestamp = strtoll(timestamp_str.c_str(), NULL, 10);
        std::string position_str = cJSON_GetObjectItem(root,"position")->valuestring;
        int64_t position = strtoll(position_str.c_str(), NULL, 10);
        UdpMRMAction action;
        action.type = recv_action;
        action.data = position_str;
        action.timestamp = (timestamp + offset);
        recv_actions.push(action);
      }
    }
  }
}

int64_t UdpMRM::get_timestamp() {
  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);
  return (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
}

int64_t UdpMRMAction::get_timestamp() {
  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);
  return (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
}

/* Add a socket to the IPV4 multicast group */
int UdpMRM::socket_add_ipv4_multicast_group(int sock, bool assign_source_if)
{
  struct ip_mreq imreq = { 0 };
  struct in_addr iaddr = { 0 };
  int retval = 0;
  // Configure source interface
  if (this->listen_all_if) {
    imreq.imr_interface.s_addr = IPADDR_ANY;
  } 
  else {
    for (auto ip : esphome::network::get_ip_addresses()) {
      if (ip.is_ip4()) {
        esph_log_d(TAG,"Multicast Source: %s",ip.str().c_str());
        inet_aton(ip.str().c_str(), &iaddr);
        inet_aton(ip.str().c_str(), &(imreq.imr_interface.s_addr));
        this_addr_ = ip.str();
        break;
      }
    }
  }

  // Configure multicast address to listen to
  retval = inet_aton(this->multicast_ipv4_addr.c_str(), &imreq.imr_multiaddr.s_addr);
  if (retval != 1) {
    esph_log_e(TAG, "Configured IPV4 multicast address '%s' is invalid.", this->multicast_ipv4_addr.c_str());
    // Errors in the return value have to be negative
    retval = -1;
    return retval;
  }
  esph_log_i(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
  if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
    esph_log_w(TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", this->multicast_ipv4_addr.c_str());
  }

  if (assign_source_if) {
    // Assign the IPv4 multicast source interface, via its IP
    // (only necessary if this socket is IPV4 only)
    retval = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
             sizeof(struct in_addr));
    if (retval < 0) {
      esph_log_e(TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
      return retval;
    }
  }

  retval = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
             &imreq, sizeof(struct ip_mreq));
  if (retval < 0) {
    esph_log_e(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
    return retval;
  }

  return ESP_OK;
}

int UdpMRM::create_multicast_ipv4_socket(void)
{
  struct sockaddr_in saddr = { 0 };
  int sock = -1;
  int retval = 0;

  sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    esph_log_e(TAG, "Failed to create socket. Error %d", errno);
    return -1;
  }

  // Bind the socket to any address
  saddr.sin_family = PF_INET;
  saddr.sin_port = htons(this->udp_port);
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  retval = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
  if (retval < 0) {
    esph_log_e(TAG, "Failed to bind socket. Error %d", errno);
    close(sock);
    return retval;
  }

  // Assign multicast TTL (set separately from normal interface TTL)
  uint8_t ttl = this->multicast_ttl;
  retval = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
  if (retval < 0) {
    esph_log_e(TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
    close(sock);
    return retval;
  }

  // no loopback
  uint8_t loopback_val = 0;
  retval = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback_val, sizeof(uint8_t));
  if (retval < 0) {
    esph_log_e(TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
    close(sock);
    return retval;
  }

  // this is also a listening socket, so add it to the multicast
  // group for listening...
  retval = socket_add_ipv4_multicast_group(sock, true);
  if (retval < 0) {
    close(sock);
    return retval;
  }

  // All set, socket is configured for sending and receiving
  return sock;
}

}  // namespace esp_adf
}  // namespace esphome
#endif
