#include "linp_doorbell.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace linp_doorbell {

static const char *const TAG = "linp_doorbell";

float LinpDoorbellComponent::get_setup_priority() const { return setup_priority::HARDWARE; }

void LinpDoorbellComponent::setup() {
  Serial2.begin(115200);
  hasSetVolume = false;
  commandQueue.push("down none");
  commandQueue.push("down none");
  commandQueue.push("down none");
  commandQueue.push("down get_volume");
  commandQueue.push("down get_switch_list");

  if (this->use_old_service_names_) {
    register_service(&LinpDoorbellComponent::setVolume, "linp_set_volume", {"volume"});
    register_service(&LinpDoorbellComponent::playTune, "linp_play_tune", {"tune"});
    register_service(&LinpDoorbellComponent::stopTune, "linp_stop_tune");
    register_service(&LinpDoorbellComponent::learnButton, "linp_learn_button", {"tune"});
    register_service(&LinpDoorbellComponent::setTune, "linp_set_tune", {"button", "tune"});
    register_service(&LinpDoorbellComponent::forgetButton, "linp_forget_button", {"button"});
    register_service(&LinpDoorbellComponent::sendRawCommand, "linp_send_raw_command", {"command"});
  } else {
    register_service(&LinpDoorbellComponent::setVolume, "set_volume", {"volume"});
    register_service(&LinpDoorbellComponent::playTune, "play_tune", {"tune"});
    register_service(&LinpDoorbellComponent::stopTune, "stop_tune");
    register_service(&LinpDoorbellComponent::learnButton, "learn_button", {"tune"});
    register_service(&LinpDoorbellComponent::setTune, "set_tune", {"button", "tune"});
    register_service(&LinpDoorbellComponent::forgetButton, "forget_button", {"button"});
    register_service(&LinpDoorbellComponent::sendRawCommand, "send_raw_command", {"command"});
  }
}

void LinpDoorbellComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Doorbell Config:");

  LOG_SENSOR("  ", "Volume", this->volume_sensor_);
  LOG_SENSOR("  ", "Chime Playing", this->chime_playing_sensor_);
}

void LinpDoorbellComponent::loop() {
  if (Serial2.available() > 0) {
    std::string received = Serial2.readStringUntil('\r').c_str();
    ESP_LOGV(TAG, "RX: %s", received.c_str());
    str::string response = handleMessage(received);
    if (response.length() > 0) {
      ESP_LOGV(TAG, "TX: %s", response.c_str());
      Serial2.print(response + "\r");
    }
  }
}

std::string LinpDoorbellComponent::handleMessage(std::string received) {
  if (isChiming) {
    isChiming = false;
    if (this->chime_playing_sensor_ != nullptr)
      this->chime_playing_sensor_->publish_state(0.0);
  }
  if (received.compare("net") == 0) {
    return ("local");
  } else if (received.compare(0,7,"result ") == 0) {
    std::string value = received.substr(7,received.length()-7);
    if (value.compare(0,4,"\"ok\"") != 0) {
      if (requests.empty()) {
        ESP_LOGD(TAG, "Unexpected property received: %s", value.c_str());
      } else {
        std::string param = requests.front();
        requests.pop();
        handleParam(param, value);
      }
    }
  } else if (received.compare(0,6,"props ") == 0) {
    std::string param = received.substr(6, received.length()-6);
    int spacePos = param.find(" ");
    std::string value = param.substr(spacePos+1,param.length()-(spacePos+1));
    param = param.substr(0, spacePos);
    ESP_LOGD(TAG, "Property received: %s = %s", param.c_str(), value.c_str());
  } else if (received.compare(0,6,"event ") == 0) {
    received = received.substr(6, received.length()-6);
    handleEvent(received);
    return ("ok");
  } else if (received.compare("get_down") == 0) {
    if (commandQueue.empty()) {
      return ("down none");
    } else {
      std::string response = commandQueue.front();
      commandQueue.pop();
      ESP_LOGD(TAG, "Sending command: %s", response.c_str());
      if (response.compare(0,9,"down get_") == 0) {
        // Strip the "down get_" prefix off.
        requests.push(response.substr(9,response.length()-9));
      } else if (response.compare(0,14,"down set_music") == 0) {
        requests.push("music");
      }
      return response;
    }
  }
  return "";
}

void LinpDoorbellComponent::handleEvent(std::string event) {
  ESP_LOGI(TAG, "Event received: %s", event.c_str());
  if (event.compare(0,15,"switch_pressed_") == 0) {
    int buttonIndex = atoi(event.substr(15, event.length()-15).c_str());
    char buttonStr[2];
    itoa(buttonIndex+1, buttonStr, 10); // Increment to get a 1-based number
    fire_homeassistant_event("esphome.linp_doorbell_button_pressed", {
      {"button", buttonStr},
      {"device", App.get_name()},
    });
  } else if(event.compare(0,9,"bell_ring") == 0) {
    event = event.substr(10,evnt.length()-10);
    if (this->chime_playing_sensor_ != nullptr)
      this->chime_playing_sensor_->publish_state(parse_number<float>(event.c_str()).value());
    isChiming = true;
    fire_homeassistant_event("esphome.linp_doorbell_tune_played", {
      {"tune", event.c_str()},
      {"device", App.get_name()},
    });
  } else if(event.compare("learn_success") == 0) {
    // Button learning succeded; request a fresh list (event supplies a list, but it's space-separated).
    // commandQueue.enqueue("down get_switch_list");
    commandQueue.push("down get_switch_list");
  }
}

void LinpDoorbellComponent::handleParam(std::string param, std::string value) {
  ESP_LOGD(TAG, "Param received: %s = %s", param.c_str(), value.c_str());
  if (param.compare("volume") == 0) {
    if (!hasSetVolume) {
      // Volume needs to be reset on boot to put it back to the last set value.
      // This also jogs the doorbell to life so that it'll actually chime on first button press.
      // If we don't do this, it won't chime on first press but will on subsequent presses.
      int initialVolume = atoi(value.c_str());
      ESP_LOGI(TAG, "Setting initial volume: %i", initialVolume);
      hasSetVolume = true;
      setVolume(initialVolume);
    }
    if (this->volume_sensor_ != nullptr)
      this->volume_sensor_->publish_state(parse_number<float>(value.c_str()).value());
  } else if (param.compare("switch_list") == 0) {
    // Comma-separated list of button tunes.
    int offset = 0;
    for(int i=0; i<10; i++) {
      int commaPos = value.find(',', offset);
      if (commaPos == std::string::npos && i < 9) {
        // Comma not found.  Value might be malformed; we won't be able to find any more tunes, so stop here.
        break;
      }
      std::string tune = value.substr(offset, commaPos-offset);
      auto tuneFloat = parse_number<float>(tune.c_str());
      if (tune.compare("255") == 0) {
        tuneFloat = -1;
      }
      //chime_sensors[i]->publish_state(tuneFloat.value());
      offset = commaPos + 1;
    }
  }
}

void LinpDoorbellComponent::setVolume(int volume) {
  if (volume < 0 || volume > 4) {
    ESP_LOGI(TAG, "Ignoring invalid volume request: %i", volume);
    return;
  }
  ESP_LOGI(TAG, "Setting volume to: %i", volume);
  std::string command = str_sprintf("down set_volume %d", volume);
  commandQueue.push(command);
  commandQueue.push("down get_volume");
}

void LinpDoorbellComponent::playTune(int tune) {
  if (tune < 1 || tune > 36) {
    ESP_LOGI(TAG, "Ignoring invalid tune request: %i", tune);
    return;
  }
  ESP_LOGI(TAG, "Playing tune: %i", tune);
  std::string command = str_sprintf("down play_specified_music %d", tune);
  commandQueue.push(command);
}

void LinpDoorbellComponent::stopTune() {
  ESP_LOGI(TAG, "Stopping tune");
  commandQueue.push("down stop_play");
}

void LinpDoorbellComponent::learnButton(int tune) {
  if (tune < 1 || tune > 36) {
    ESP_LOGI(TAG, "Ignoring learn request - invalid tune: %i", tune);
    return;
  }
  ESP_LOGI(TAG, "Entering learn mode with tune: %i", tune);
  std::string command = str_sprintf("down enter_specified_learn_mode %d", tune);
  commandQueue.push(command);
}

void LinpDoorbellComponent::setTune(int button, int tune) {
  if (button < 1 || button > 10) {
    ESP_LOGI(TAG, "Ignoring set tune request - invalid button: %i", button);
    return;
  }
  if (tune < 1 || tune > 36) {
    ESP_LOGI(TAG, "Ignoring set tune request - invalid tune: %i", tune);
    return;
  }
  ESP_LOGI(TAG, "Setting button %i to tune: %i", button, tune);
  std::string command = str_sprintf("down set_music_for_switch %d, %d", button-1, tune);
  commandQueue.push(command);
}

void LinpDoorbellComponent::forgetButton(int button) {
  if (button < 1 || button > 10) {
    ESP_LOGI(TAG, "Ignoring forget button request - invalid button: %i", button);
    return;
  }
  ESP_LOGI(TAG, "Forgetting button %i", button);
  std::string command = str_sprintf("down delete_specified_switch %d", button-1);
  commandQueue.push(command);
  // Doorbell sends a "switch list" param after forgetting the button.
  requests.push("switch_list");
}

void LinpDoorbellComponent::sendRawCommand(std::string command) {
  ESP_LOGI(TAG, "Sending raw command: %s", command.c_str());
  commandQueue.push(command);
}

}  // namespace linp_doorbell
}  // namespace esphome
