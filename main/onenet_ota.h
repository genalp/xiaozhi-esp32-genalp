#ifndef _ONENET_OTA_H_
#define _ONENET_OTA_H_

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

class OnenetOta {
public:
    OnenetOta();
    ~OnenetOta();

    /**
     * @brief 上报当前版本
     * 
     * @return true 上报成功
     * @return false 上报失败
     * 
     * @note https://open.iot.10086.cn/doc/aiot/fuse/detail/1454
     */
    bool ReportVersion();

    /**
     * @brief 检测升级任务
     * 
     * @return true 有升级任务
     * @return false 没有升级任务
     * 
     * @note https://open.iot.10086.cn/doc/aiot/fuse/detail/1447
     */
    bool CheckTask();

    /**
     * @brief 上报升级状态
     * 
     * @return true 上报成功
     * @return false 上报失败
     * 
     * @note https://open.iot.10086.cn/doc/aiot/fuse/detail/1449
     */
    bool ReportStatus(int32_t status);

    /**
     * @brief 升级
     * 
     * @return true 升级成功
     * @return false 升级失败
     * 
     * @note https://open.iot.10086.cn/doc/aiot/fuse/detail/1448
     */
    bool Upgrade(std::function<void(int progress, size_t speed)> callback);

    bool HasNewVersion() { return has_ota_task_; }
    std::string GetFirmwareVersion() { return target_version_;}

private:
    std::string product_id_;
    std::string device_id_;
    std::string authorization_;
    
    bool has_ota_task_;
    std::string tid_;
    std::string target_version_;
    std::string target_md5_;
    int32_t target_size_;
    int32_t download_size_;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::unique_ptr<Http> SetupHttp();
};


#endif // _ONENET_OTA_H_
