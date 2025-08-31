#include "onenet_ota.h"
#include "version.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <mbedtls/md5.h> 


#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "OnenetOta"


OnenetOta::OnenetOta() {
    Settings settings("onenet", true);
    // 以下为首次初始化时写入信息
    // settings.SetString("product_id", "aaa");
    // settings.SetString("device_id", "bbb");
    // settings.SetString("ota_auth", "ccc");

    // 读取设置信息
    authorization_ = settings.GetString("ota_auth");
    product_id_ = settings.GetString("product_id");
    device_id_ = settings.GetString("device_id");
}

OnenetOta::~OnenetOta() {
}


// 鉴权信息: https://open.iot.10086.cn/doc/aiot/fuse/detail/1464
std::unique_ptr<Http> OnenetOta::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("Authorization", authorization_);
    http->SetHeader("Content-Type", "application/json");
    return http;
}


bool OnenetOta::ReportVersion() {
    ESP_LOGE(TAG, "CurrentOneNetVersion: %s", APP_VERSION);
    std::string url = "https://iot-api.heclouds.com/fuse-ota/" + product_id_ + "/" + device_id_ + "/version";
    auto http = SetupHttp();

    std::string data = "{\"s_version\":\"" + std::string(APP_VERSION) + "\",\"f_version\":\"1.0.0\"}";
    ESP_LOGE(TAG, "ReportVersion: %s", data.c_str());
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return false;
    }

    data = http->ReadAll();
    ESP_LOGE(TAG, "GetRequest: %s", data.c_str());

    http->Close();

    return true;
}


bool OnenetOta::CheckTask() {
    std::string url = "https://iot-api.heclouds.com/fuse-ota/" + product_id_ + "/" + device_id_ + "/check?type=2&version=" + std::string(APP_VERSION);
    auto http = SetupHttp();

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return false;
    }

    std::string data = http->ReadAll();
    ESP_LOGE(TAG, "GetRequest: %s", data.c_str());
    http->Close();

    /*
    {
        "code": 0,
        "msg": "succ",
        "data": {
            "target": "0.0.3",
            "tid": 1266435,
            "size": 3871360,
            "md5": "761bf238d3b3cbc718ed0e84191bd70c",
            "status": 1,
            "type": 1
        },
        "request_id": "59db175991ef4b7c8551e6d2fa6affe4"
    }
    */
   
    // 解析升级信息
    has_ota_task_ = false;
    tid_.clear();
    target_version_.clear();
    target_size_ = 0;
    target_md5_.clear();

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }
    
    // 检查是否有升级包
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    if (cJSON_IsString(msg)) {
        ESP_LOGE(TAG, "CheckTask, msg: %s", msg->valuestring);
    }
    // code为0才是有升级任务
    if (cJSON_IsNumber(code)) {
        if (code->valueint != 0) {
            ESP_LOGE(TAG, "No OTA task, code: %d", code->valueint);
            return false;
        } else {
            has_ota_task_ = true;
        }
    }
    // 解析升级信息
    cJSON *ota_data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(ota_data)) {
        cJSON *target = cJSON_GetObjectItem(ota_data, "target");
        if (cJSON_IsString(target)) {
            target_version_ = target->valuestring;
        }
        cJSON *tid = cJSON_GetObjectItem(ota_data, "tid");
        if (cJSON_IsNumber(tid)) {
            tid_ = std::to_string(tid->valueint);
        }
        cJSON *size = cJSON_GetObjectItem(ota_data, "size");
        if (cJSON_IsNumber(size)) {
            target_size_ = size->valueint;
        }
        cJSON *md5 = cJSON_GetObjectItem(ota_data, "md5");
        if (cJSON_IsString(md5)) {
            target_md5_ = md5->valuestring;
        }
        ESP_LOGE(TAG, "OtaTask, target_version: %s, tid: %s, target_size: %d, target_md5: %s", target_version_.c_str(), tid_.c_str(), target_size_, target_md5_.c_str());
    }

    return true;
}


bool OnenetOta::ReportStatus(int32_t status) {
    std::string url = "https://iot-api.heclouds.com/fuse-ota/" + product_id_ + "/" + device_id_ + "/" + tid_ + "/status";
    auto http = SetupHttp();

    std::string data = "{\"step\":" + std::to_string(status) + "}";
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return false;
    }

    data = http->ReadAll();
    ESP_LOGE(TAG, "GetRequest: %s", data.c_str());
    http->Close();

    return true;
}

bool OnenetOta::Upgrade(std::function<void(int progress, size_t speed)> callback) {
    if (!has_ota_task_) {
        ESP_LOGE(TAG, "No OTA task");
        return false;
    }

    upgrade_callback_ = callback;

    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto http = SetupHttp();
    std::string url = "https://iot-api.heclouds.com/fuse-ota/" + product_id_ + "/" + device_id_ + "/" + tid_ + "/download";
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    if (content_length != target_size_) {
        ESP_LOGE(TAG, "Content length mismatch: expected %d, got %d", target_size_, content_length);
        return false;
    }

    mbedtls_md5_context md5_ctx;
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts(&md5_ctx);
    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            mbedtls_md5_free(&md5_ctx);
            return false;
        }

        mbedtls_md5_update(&md5_ctx, (const unsigned char*)buffer, ret);

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGE(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (!image_header_checked) {
            image_header.append(buffer, ret);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                // 宏定义版本号无法体现在固件头部，校验会出错
                // auto current_version = esp_app_get_description()->version;
                // if (memcmp(new_app_info.version, current_version, sizeof(new_app_info.version)) == 0) {
                //     ESP_LOGE(TAG, "Firmware version is the same, skipping upgrade");
                //     return false;
                // }

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }
        auto err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return false;
        }
    }
    http->Close();

    // 完成MD5计算
    unsigned char calculated_md5[16];
    mbedtls_md5_finish(&md5_ctx, calculated_md5);
    mbedtls_md5_free(&md5_ctx);

    // 将计算出的MD5转换为十六进制字符串
    char calculated_md5_str[33];
    for (int i = 0; i < 16; i++) {
        sprintf(calculated_md5_str + i * 2, "%02x", calculated_md5[i]);
    }
    calculated_md5_str[32] = '\0';

    ESP_LOGI(TAG, "Calculated MD5: %s", calculated_md5_str);
    ESP_LOGI(TAG, "Expected MD5: %s", target_md5_.c_str());

    // 比较MD5值
    if (strcasecmp(calculated_md5_str, target_md5_.c_str()) != 0) {
        ESP_LOGE(TAG, "MD5 verification failed!");
        esp_ota_abort(update_handle);
        return false;
    }

    ESP_LOGI(TAG, "MD5 verification passed!");

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}
