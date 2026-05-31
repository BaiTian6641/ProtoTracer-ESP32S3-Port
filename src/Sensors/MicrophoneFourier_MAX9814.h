//Configured for 60dB gain

//#define ARM_MATH_CM4

#include <Arduino.h>
//#include <arm_math.h>
//#include <IntervalTimer.h>
#include "esp_dsp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "../Math/Mathematics.h"
#include "MicrophoneSimple_MAX9814.h"
#include "../Filter/DerivativeFilter.h"
#include "../Filter/FFTFilter.h"
#include "../Filter/PeakDetection.h"
#include "../Signals/TimeStep.h"
//volatile SemaphoreHandle_t timerSemaphore;
static const char *TAG = "main";

#ifndef MIC_FFT_ASYNC
#define MIC_FFT_ASYNC 1
#endif

#ifndef MIC_FFT_TASK_CORE
#define MIC_FFT_TASK_CORE 0
#endif

#ifndef MIC_FFT_TASK_STACK_BYTES
#define MIC_FFT_TASK_STACK_BYTES 6144
#endif

class MicrophoneFourierIT{
private:
    //static IntervalTimer sampleTimer;
    //static hw_timer_t * timer;
    static TimeStep timeStep;

    static esp_timer_handle_t timer;
    static const esp_timer_create_args_t timerParameters;
    static TaskHandle_t fftTaskHandle;
    static SemaphoreHandle_t samplesReadySemaphore;
    static portMUX_TYPE outputMux;

    
    //static esp_err_t ret;
    //static const char *TAG;

    static const uint16_t FFTSize = 256;
    static const uint8_t OutputBins = 128;
    static uint16_t sampleRate;
    static uint16_t samples;
    static volatile uint16_t samplesStorage;
    static uint8_t pin;
    static float minDB;
    static float maxDB;
    static float threshold;
    static float currentValue;
    static float refreshRate;
    static volatile bool samplesReady;
    static bool isInitialized;
    static bool asyncTaskStarted;
    static volatile bool outputPending;
    static volatile uint32_t processedFrameCount;
    static volatile uint32_t samplerDropCount;
    static DerivativeFilter peakFilterRate;

    static uint16_t frequencyBins[OutputBins];
    static float* SampleArray;
    static float* inputStorage;
    static float* outputData;
    static float* outputDataFilt;
    static float* pendingInputStorage;
    static float* pendingOutputData;
    static float* pendingOutputDataFilt;
    static float* publishedInputStorage;
    static float* publishedOutputData;
    static float* publishedOutputDataFilt;
    static float* noiseMagnitude;
    static float pendingThreshold;
    static float publishedThreshold;
    static FFTFilter fftFilters[OutputBins];

    
    //static arm_cfft_radix4_instance_f32 RadixFFT;

    static float AverageMagnitude(uint16_t binL, uint16_t binH){
        float average = 0.0f;

        for (uint16_t i = 1; i < FFTSize; i++){
            if (i >= binL && i <= binH) average += SampleArray[i*2];
        }

        return average / float(binH - binL + 1);
    }

    static void SamplerCallback(void*){
        if (samplesReady || samplesStorage >= FFTSize) {
            samplerDropCount = samplerDropCount + 1;
            return;
        }

        const uint16_t storageIndex = samplesStorage;
        inputStorage[storageIndex] = (float)analogRead(pin);
        samplesStorage = storageIndex + 1;

        if(samplesStorage >= FFTSize){
            if (timer) {
                esp_timer_stop(timer);
            }
            samplesReady = true;
            if (asyncTaskStarted && samplesReadySemaphore) {
                xSemaphoreGive(samplesReadySemaphore);
            }
        }
    }

    static void StartSampler(){
        samplesReady = false;
        samples = 0;
        samplesStorage = 0;

        // Timer is created once during Initialize; only start/stop it per cycle.
        if (!timer) {
            // Timer creation must have failed during Initialize — disable audio path
            samplesReady = true;
            return;
        }
        esp_err_t err = esp_timer_start_periodic(timer, 1000000 / sampleRate);
        if (err != ESP_OK) {
            samplesReady = true;
        }
    }

    static void PublishOutputFrame(){
        portENTER_CRITICAL(&outputMux);
        memcpy(pendingInputStorage, inputStorage, sizeof(pendingInputStorage));
        memcpy(pendingOutputData, outputData, sizeof(pendingOutputData));
        memcpy(pendingOutputDataFilt, outputDataFilt, sizeof(pendingOutputDataFilt));
        pendingThreshold = threshold;
        outputPending = true;
        portEXIT_CRITICAL(&outputMux);
    }

    static void SnapshotOutputFrame(){
        if (!outputPending) return;

        portENTER_CRITICAL(&outputMux);
        memcpy(publishedInputStorage, pendingInputStorage, sizeof(publishedInputStorage));
        memcpy(publishedOutputData, pendingOutputData, sizeof(publishedOutputData));
        memcpy(publishedOutputDataFilt, pendingOutputDataFilt, sizeof(publishedOutputDataFilt));
        publishedThreshold = pendingThreshold;
        outputPending = false;
        portEXIT_CRITICAL(&outputMux);
    }

    static void ProcessReadySamples(){
        if (!samplesReady) return;

        samplesReady = false;

        for(int i = 0; i< FFTSize; i++){
            SampleArray[i*2 + 0] = inputStorage[i];
            SampleArray[i*2 + 1] = 0;
        }

        for (int i = 0; i < FFTSize; ++i) {
            float real = SampleArray[i * 2];
            float imag = SampleArray[i * 2 + 1];
            float magnitude = sqrtf(real * real + imag * imag);
            float phase = atan2f(imag, real);
        
            float subtractedMag = magnitude - noiseMagnitude[i];
            if (subtractedMag < 0.0f) subtractedMag = 0.0f;
        
            SampleArray[i * 2] = subtractedMag * cosf(phase);
            SampleArray[i * 2 + 1] = subtractedMag * sinf(phase);
        }
        
        dsps_fft4r_fc32(SampleArray, FFTSize);
        dsps_cplx2real_fc32(SampleArray, FFTSize);
        float averageMagnitude = 0.0f;

        for (uint8_t i = 0; i < OutputBins - 1; i++){
            float intensity = 20.0f * log10f(AverageMagnitude(i, i + 1));

            intensity = map(intensity, minDB, maxDB, 0.0f, 1.0f);
            outputData[i] = intensity;
            outputDataFilt[i] = (fftFilters[i].Filter(intensity) / 3.5);
            if (i % 12 == 0) averageMagnitude = peakFilterRate.Filter(inputStorage[i] / 4096.0f);
        }

        averageMagnitude *= 10.0f;
        threshold = powf(averageMagnitude, 2.0f);
        threshold = threshold > 0.2f ? (threshold * 5.0f > 1.0f ? 1.0f : threshold * 5.0f) : 0.0f;
        
        PublishOutputFrame();
        processedFrameCount = processedFrameCount + 1;
        Reset();
    }

    static void FftTask(void*){
        while (true) {
            if (samplesReadySemaphore && xSemaphoreTake(samplesReadySemaphore, portMAX_DELAY) == pdTRUE) {
                ProcessReadySamples();
                StartSampler();
            }
        }
    }

    static void EstimateNoiseProfile() {
        const int noiseFrames = 50;
        memset(noiseMagnitude, 0, FFTSize * sizeof(float));
    
        for (int f = 0; f < noiseFrames; ++f) {
            for (int i = 0; i < FFTSize; ++i) {
                inputStorage[i] = (float)analogRead(pin);
                SampleArray[i * 2] = inputStorage[i];
                SampleArray[i * 2 + 1] = 0.0f;
            }
    
            dsps_fft4r_fc32(SampleArray, FFTSize);
            dsps_bit_rev4r_fc32(SampleArray, FFTSize);
    
            for (int i = 0; i < FFTSize; ++i) {
                float real = SampleArray[i * 2];
                float imag = SampleArray[i * 2 + 1];
                noiseMagnitude[i] += sqrtf(real * real + imag * imag);
            }
        }
    
        for (int i = 0; i < FFTSize; ++i) {
            noiseMagnitude[i] /= noiseFrames;
        }
    }
    

public:
    static void Initialize(uint8_t pin, uint16_t sampleRate, float minDB, float maxDB, float refreshRate = 60.0f){
        MicrophoneFourierIT::minDB = minDB;
        MicrophoneFourierIT::maxDB = maxDB;
        MicrophoneFourierIT::pin = pin;
        MicrophoneFourierIT::refreshRate = refreshRate;
        // Serial already initialized by main.cpp setup()
        dsps_fft4r_init_fc32(NULL, FFTSize);
        //Serial.print("OK");

        pinMode(pin, INPUT);
        analogReadResolution(12);

        // Create the timer handle once; StartSampler will start/stop it per cycle.
        esp_err_t err = esp_timer_create(&timerParameters, &timer);
        if (err != ESP_OK) {
            Serial.printf("[MIC] Timer create failed: %d\n", err);
            timer = nullptr;
        }

        MicrophoneFourierIT::sampleRate = sampleRate;
        MicrophoneFourierIT::samples = 0;
        MicrophoneFourierIT::samplesReady = false;
        MicrophoneFourierIT::outputPending = false;
        MicrophoneFourierIT::processedFrameCount = 0;
        MicrophoneFourierIT::samplerDropCount = 0;
        MicrophoneFourierIT::threshold = 0.0f;
        MicrophoneFourierIT::pendingThreshold = 0.0f;
        MicrophoneFourierIT::publishedThreshold = 0.0f;

        // Allocate FFT working buffers in PSRAM to reduce internal DRAM pressure.
        // ESP-DSP FFT uses CPU (not DMA), so PSRAM via cache is safe.
        #define MIC_PSRAM_ALLOC(sz) heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        SampleArray        = (float*)MIC_PSRAM_ALLOC(FFTSize * 2 * sizeof(float));
        inputStorage       = (float*)MIC_PSRAM_ALLOC(FFTSize * sizeof(float));
        outputData         = (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        outputDataFilt     = (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        pendingInputStorage  = (float*)MIC_PSRAM_ALLOC(FFTSize * sizeof(float));
        pendingOutputData    = (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        pendingOutputDataFilt= (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        publishedInputStorage  = (float*)MIC_PSRAM_ALLOC(FFTSize * sizeof(float));
        publishedOutputData    = (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        publishedOutputDataFilt= (float*)MIC_PSRAM_ALLOC(OutputBins * sizeof(float));
        noiseMagnitude     = (float*)MIC_PSRAM_ALLOC(FFTSize * sizeof(float));
        #undef MIC_PSRAM_ALLOC

        if (!SampleArray || !inputStorage || !outputData || !noiseMagnitude) {
            Serial.println("[MIC] PSRAM alloc failed — audio disabled");
            isInitialized = false;
            return;
        }

        float windowRange = float(sampleRate) / 2.0f / float(OutputBins);

        timeStep.SetFrequency(refreshRate);

        for (uint8_t i = 0; i < OutputBins; i++){
            float frequency = (float(i) * windowRange);
            frequencyBins[i] = uint16_t(frequency / float(sampleRate / FFTSize));
        }
        
        EstimateNoiseProfile();
        
        // Free noise profile after estimation — not needed at runtime
        heap_caps_free(noiseMagnitude);
        noiseMagnitude = nullptr;
        
        isInitialized = true;

#if MIC_FFT_ASYNC
        if (!samplesReadySemaphore) {
            samplesReadySemaphore = xSemaphoreCreateBinary();
        }
        if (samplesReadySemaphore && !fftTaskHandle) {
            BaseType_t taskResult = xTaskCreatePinnedToCore(
                FftTask,
                "MicFFT",
                MIC_FFT_TASK_STACK_BYTES,
                nullptr,
                1,
                &fftTaskHandle,
                MIC_FFT_TASK_CORE);
            asyncTaskStarted = (taskResult == pdPASS);
            if (!asyncTaskStarted) {
                Serial.println("[MIC] Async FFT task create failed; using main-loop fallback");
            }
        }
#endif

        // Start the first sampling cycle
        if (timer) {
            StartSampler();
        }

        Serial.printf("MICROPHONE-OK async=%u core=%d ", asyncTaskStarted ? 1 : 0, MIC_FFT_TASK_CORE);
    }

    static bool IsInitialized(){
        return isInitialized;
    }

    static float GetSampleRate(){
        return sampleRate;
    }

    static float* GetSamples(){
        return publishedInputStorage;
    }

    static float* GetFourier(){
        return publishedOutputData;
    }

    static float* GetFourierFiltered(){
        return publishedOutputDataFilt;
    }

    static float GetCurrentMagnitude(){
        //Serial.print(threshold);
        return publishedThreshold;
    }

    static uint32_t GetProcessedFrameCount(){
        return processedFrameCount;
    }

    static uint32_t GetSamplerDropCount(){
        return samplerDropCount;
    }

    static uint32_t GetTaskStackHighWater(){
        return fftTaskHandle ? uxTaskGetStackHighWaterMark(fftTaskHandle) : 0;
    }

    static bool IsAsyncEnabled(){
        return asyncTaskStarted;
    }

    static void Reset(){
        for(int i = 0; i < FFTSize*2; i++){
            SampleArray[i] = 0.0f;
        }
    }
    
    static void Update(){
        if (!asyncTaskStarted && samplesReady && timeStep.IsReady()) {
            ProcessReadySamples();
            StartSampler();
        }

        SnapshotOutputFrame();
    }
};

//IntervalTimer MicrophoneFourierIT::sampleTimer;
TimeStep MicrophoneFourierIT::timeStep = TimeStep(60);

const uint16_t MicrophoneFourierIT::FFTSize;
const uint8_t MicrophoneFourierIT::OutputBins;
uint16_t MicrophoneFourierIT::sampleRate = 8000;
uint16_t MicrophoneFourierIT::samples = 0;
volatile uint16_t MicrophoneFourierIT::samplesStorage = 0;
uint8_t MicrophoneFourierIT::pin = 11;
float MicrophoneFourierIT::minDB = 50.0f;
float MicrophoneFourierIT::maxDB = 120.0f;
float MicrophoneFourierIT::threshold = 400.0f;
float MicrophoneFourierIT::refreshRate = 60.0f;
volatile bool MicrophoneFourierIT::samplesReady = false;
bool MicrophoneFourierIT::isInitialized = false;
bool MicrophoneFourierIT::asyncTaskStarted = false;
volatile bool MicrophoneFourierIT::outputPending = false;
volatile uint32_t MicrophoneFourierIT::processedFrameCount = 0;
volatile uint32_t MicrophoneFourierIT::samplerDropCount = 0;
DerivativeFilter MicrophoneFourierIT::peakFilterRate;
TaskHandle_t MicrophoneFourierIT::fftTaskHandle = nullptr;
SemaphoreHandle_t MicrophoneFourierIT::samplesReadySemaphore = nullptr;
portMUX_TYPE MicrophoneFourierIT::outputMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t MicrophoneFourierIT::frequencyBins[];
float* MicrophoneFourierIT::SampleArray = nullptr;
float* MicrophoneFourierIT::inputStorage = nullptr;
float* MicrophoneFourierIT::outputData = nullptr;
float* MicrophoneFourierIT::outputDataFilt = nullptr;
float* MicrophoneFourierIT::pendingInputStorage = nullptr;
float* MicrophoneFourierIT::pendingOutputData = nullptr;
float* MicrophoneFourierIT::pendingOutputDataFilt = nullptr;
float* MicrophoneFourierIT::publishedInputStorage = nullptr;
float* MicrophoneFourierIT::publishedOutputData = nullptr;
float* MicrophoneFourierIT::publishedOutputDataFilt = nullptr;
float* MicrophoneFourierIT::noiseMagnitude = nullptr;
float MicrophoneFourierIT::pendingThreshold = 0.0f;
float MicrophoneFourierIT::publishedThreshold = 0.0f;
FFTFilter MicrophoneFourierIT::fftFilters[];

//arm_cfft_radix4_instance_f32 MicrophoneFourierIT::RadixFFT;

//hw_timer_t* MicrophoneFourierIT::timer = NULL;
//portMUX_TYPE MicrophoneFourierIT::timerMux = portMUX_INITIALIZER_UNLOCKED;
//const char* MicrophoneFourierIT::TAG = "main";
esp_timer_handle_t MicrophoneFourierIT::timer;
const esp_timer_create_args_t MicrophoneFourierIT::timerParameters = { .callback = &MicrophoneFourierIT::SamplerCallback, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "micSampler" };

