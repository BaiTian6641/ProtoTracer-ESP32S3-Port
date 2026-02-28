#pragma once

#include "../Render/CameraBase.h"

#ifndef RENDER_MULTICORE_CAMERA
#define RENDER_MULTICORE_CAMERA 0
#endif

#if RENDER_MULTICORE_CAMERA && defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

class Controller {
private:
    const float softStart = 3000000;//microseconds
    long previousTime;
    CameraBase** cameras;
    uint8_t count = 0;
    float renderTime = 0.0f;
    uint8_t maxBrightness;
    uint8_t maxAccentBrightness;
    bool isOn = false;

#if RENDER_MULTICORE_CAMERA && defined(ARDUINO_ARCH_ESP32)
    TaskHandle_t rasterWorkerTask = nullptr;
    TaskHandle_t renderCallerTask = nullptr;
    Scene* rasterWorkerScene = nullptr;
    volatile bool rasterWorkerBusy = false;
    volatile bool rasterWorkerStop = false;
    uint8_t rasterWorkerCameraIndex = 1;

    static void RasterWorkerEntry(void* param){
        Controller* self = static_cast<Controller*>(param);

        for(;;){
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            if (self->rasterWorkerStop) {
                break;
            }

            Scene* scene = self->rasterWorkerScene;
            const uint8_t cameraIndex = self->rasterWorkerCameraIndex;

            if (scene && cameraIndex < self->count) {
                self->cameras[cameraIndex]->Rasterize(scene);

                if(scene->UseEffect()){
                    scene->GetEffect()->ApplyEffect(self->cameras[cameraIndex]->GetPixelGroup());
                }
            }

            self->rasterWorkerBusy = false;

            if (self->renderCallerTask) {
                xTaskNotifyGive(self->renderCallerTask);
            }
        }

        vTaskDelete(nullptr);
    }

    void InitRasterWorker(){
        if (rasterWorkerTask || count < 2) return;

        const BaseType_t currentCore = xPortGetCoreID();
        const BaseType_t workerCore = currentCore == 0 ? 1 : 0;
        xTaskCreatePinnedToCore(
            RasterWorkerEntry,
            "RasterWorker",
            8192,
            this,
            2,
            &rasterWorkerTask,
            workerCore
        );
    }
#endif

    void UpdateBrightness(){
        if (!isOn && previousTime < softStart){
            brightness = map(previousTime, 0, softStart, 0, maxBrightness);
            accentBrightness = map(previousTime, 0, softStart, 0, maxAccentBrightness);
        }
        else if (!isOn){
            brightness = maxBrightness;
            accentBrightness = maxAccentBrightness;
            isOn = true;
        }
    }

protected:
    uint8_t brightness;
    uint8_t accentBrightness;

    Controller(CameraBase** cameras, uint8_t count, uint8_t maxBrightness, uint8_t maxAccentBrightness){
        this->cameras = cameras;
        this->count = count;
        this->maxBrightness = maxBrightness;
        this->maxAccentBrightness = maxAccentBrightness;
        previousTime = micros();

#if RENDER_MULTICORE_CAMERA && defined(ARDUINO_ARCH_ESP32)
        InitRasterWorker();
#endif
    }

    virtual ~Controller(){
#if RENDER_MULTICORE_CAMERA && defined(ARDUINO_ARCH_ESP32)
        if (rasterWorkerTask) {
            rasterWorkerStop = true;
            xTaskNotifyGive(rasterWorkerTask);
            vTaskDelay(1);
            rasterWorkerTask = nullptr;
        }
#endif
    }

public:
    CameraBase** GetCameras(){
        return cameras;
    }

    uint8_t GetCameraCount(){
        return count;
    }

    void Render(Scene* scene){
        previousTime = micros();

        UpdateBrightness();

#if RENDER_MULTICORE_CAMERA && defined(ARDUINO_ARCH_ESP32)
        if (count > 1 && rasterWorkerTask) {
            renderCallerTask = xTaskGetCurrentTaskHandle();
            rasterWorkerScene = scene;
            rasterWorkerCameraIndex = 1;
            rasterWorkerBusy = true;
            xTaskNotifyGive(rasterWorkerTask);

            cameras[0]->Rasterize(scene);
            if(scene->UseEffect()){
                scene->GetEffect()->ApplyEffect(cameras[0]->GetPixelGroup());
            }

            for (int i = 2; i < count; i++){
                cameras[i]->Rasterize(scene);
                if(scene->UseEffect()){
                    scene->GetEffect()->ApplyEffect(cameras[i]->GetPixelGroup());
                }
            }

            if (rasterWorkerBusy) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }

            renderCallerTask = nullptr;
            renderTime = ((float)(micros() - previousTime)) / 1000000.0f;
            return;
        }
#endif

        for (int i = 0; i < count; i++){
            cameras[i]->Rasterize(scene);

            if(scene->UseEffect()){
                scene->GetEffect()->ApplyEffect(cameras[i]->GetPixelGroup());
            }
        }
        

        renderTime = ((float)(micros() - previousTime)) / 1000000.0f;
    }

    void RenderCamera(Scene* scene, uint8_t cameraNum){
        previousTime = micros();

        UpdateBrightness();

        cameras[cameraNum]->Rasterize(scene);

        if(scene->UseEffect()){
            scene->GetEffect()->ApplyEffect(cameras[cameraNum]->GetPixelGroup());
        }

        renderTime = ((float)(micros() - previousTime)) / 1000000.0f;
    }

    void SetBrightness(uint8_t maxBrightness){
        this->maxBrightness = maxBrightness;
        
        if(isOn){//past soft start
            this->brightness = maxBrightness;
        }
    }

    void SetAccentBrightness(uint8_t maxAccentBrightness){
        this->maxAccentBrightness = maxAccentBrightness;
        
        if(isOn){//past soft start
            this->accentBrightness = maxAccentBrightness;
        }
    }

    virtual void Initialize() = 0;
    virtual void Display() = 0;

    float GetRenderTime(){
        return renderTime;
    }

};
