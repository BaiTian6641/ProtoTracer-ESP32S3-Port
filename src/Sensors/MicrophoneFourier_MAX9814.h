//Configured for 60dB gain

//#define ARM_MATH_CM4

#include <Arduino.h>
//#include <arm_math.h>
//#include <IntervalTimer.h>
#include "esp_dsp.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "..\Math\Mathematics.h"
#include "MicrophoneSimple_MAX9814.h"
#include "..\Filter\DerivativeFilter.h"
#include "..\Filter\FFTFilter.h"
#include "..\Filter\PeakDetection.h"
#include "..\Signals\TimeStep.h"
//volatile SemaphoreHandle_t timerSemaphore;
static const char *TAG = "main";

class MicrophoneFourierIT{
private:
    //static IntervalTimer sampleTimer;
    //static hw_timer_t * timer;
    static TimeStep timeStep;

    static esp_timer_handle_t timer;
    static const esp_timer_create_args_t timerParameters;

    
    //static esp_err_t ret;
    //static const char *TAG;

    static const uint16_t FFTSize = 256;
    static const uint8_t OutputBins = 128;
    static uint16_t sampleRate;
    static uint16_t samples;
    static uint16_t samplesStorage;
    static uint8_t pin;
    static float minDB;
    static float maxDB;
    static float threshold;
    static float currentValue;
    static float refreshRate;
    static bool samplesReady;
    static bool isInitialized;
    static DerivativeFilter peakFilterRate;

    static uint16_t frequencyBins[OutputBins];
    static float SampleArray[FFTSize*2];
    static int SampleArrayInt[FFTSize * 2];
    static float inputStorage[FFTSize];
    static int inputStorageInt[FFTSize];
    static float outputMagn[FFTSize];
    static float outputData[OutputBins];
    static float outputDataFilt[OutputBins];
    static FFTFilter fftFilters[OutputBins];
    
    //static arm_cfft_radix4_instance_f32 RadixFFT;

    static float AverageMagnitude(uint16_t binL, uint16_t binH){
        float average = 0.0f;

        for (uint16_t i = 1; i < FFTSize; i++){
            if (i >= binL && i <= binH) average += SampleArray[i*2];
        }

        return average / float(binH - binL + 1);
    }

    static void SamplerCallback(){
        int inputSample = analogRead(pin);

        inputStorageInt[samplesStorage++] = inputSample;

        if(samplesStorage >= FFTSize){
            esp_timer_stop(timer);
            esp_timer_delete(timer);
            samplesReady = true;
        }
    }

    static void StartSampler(){
        samplesReady = false;
        samples = 0;
        samplesStorage = 0;

        esp_timer_create(&timerParameters, &timer);
        esp_timer_start_periodic(timer, 1000000 / sampleRate);
        Serial0.print("O0K");
    }

public:
    static void Initialize(uint8_t pin, uint16_t sampleRate, float minDB, float maxDB, float refreshRate = 60.0f){
        MicrophoneFourierIT::minDB = minDB;
        MicrophoneFourierIT::maxDB = maxDB;
        MicrophoneFourierIT::pin = pin;
        MicrophoneFourierIT::refreshRate = refreshRate;
        Serial0.begin(115200);
        //Serial0.print("OK");

        pinMode(pin, INPUT);
        analogReadResolution(12);

        StartSampler();

        MicrophoneFourierIT::sampleRate = sampleRate;
        MicrophoneFourierIT::samples = 0;
        MicrophoneFourierIT::samplesReady = false;

        float windowRange = float(sampleRate) / 2.0f / float(OutputBins);

        timeStep.SetFrequency(refreshRate);

        for (uint8_t i = 0; i < OutputBins; i++){
            float frequency = (float(i) * windowRange);
            frequencyBins[i] = uint16_t(frequency / float(sampleRate / FFTSize));
        }
        

        
        isInitialized = true;
        Serial0.print("MICROPHONE-OK ");
    }

    static bool IsInitialized(){
        return isInitialized;
    }

    static float GetSampleRate(){
        return sampleRate;
    }

    static float* GetSamples(){
        return inputStorage;
    }

    static float* GetFourier(){
        return outputData;
    }

    static float* GetFourierFiltered(){
        return outputDataFilt;
    }

    static float GetCurrentMagnitude(){
        //Serial0.print(threshold);
        return threshold;
    }

    static void Reset(){
        for(int i = 0; i < FFTSize*2; i++){
            SampleArray[i] = 0.0f;
        }
    }
    
    static void Update(){
        Serial0.print(timeStep.IsReady());
        if(!samplesReady && timeStep.IsReady()) return;

        for(int i = 0; i< FFTSize; i++){
            inputStorage[i] = (float)inputStorageInt[i];
            SampleArray[i*2 + 0] = (float)inputStorageInt[i];
            SampleArray[i*2 + 1] = 0;
        }

        //arm_cfft_radix4_init_f32(&RadixFFT, FFTSize, 0, 1);
        //arm_cfft_radix4_f32(&RadixFFT, inputSamp);
        //arm_cmplx_mag_f32(inputSamp, outputMagn, FFTSize);
        //Serial0.print(" OK ");
        
        //Serial0.print(" O--K ");
        dsps_fft4r_init_fc32(NULL, FFTSize);
        dsps_fft4r_fc32(SampleArray,FFTSize);
        dsps_bit_rev4r_fc32(SampleArray, FFTSize);
        dsps_cplx2real_fc32(SampleArray, FFTSize);

        //for (int i = 0 ; i < FFTSize ; i++) {
        //outputMagn[i] = 10 * log10f((SampleArray[i * 2 + 0] * SampleArray[i * 2 + 0] + SampleArray[i * 2 + 1] * SampleArray[i * 2 + 1]+ 0.0000001)/FFTSize);
        //}//
        
        //Serial0.println(inputStorage[0]);
        Serial0.print(" DSPOK ");
        float averageMagnitude = 0.0f;

        for (uint8_t i = 0; i < OutputBins - 1; i++){
            float intensity = 20.0f * log10f(AverageMagnitude(i, i + 1));

            intensity = map(intensity, minDB, maxDB, 0.0f, 1.0f);

            //Serial0.println(intensity);
            
            outputData[i] = intensity;
            outputDataFilt[i] = fftFilters[i].Filter(intensity);
            if (i % 12 == 0) averageMagnitude = peakFilterRate.Filter(inputStorage[i] / 4096.0f);
        }

        averageMagnitude *= 10.0f;
        threshold = powf(averageMagnitude, 2.0f);
        threshold = threshold > 0.2f ? (threshold * 5.0f > 1.0f ? 1.0f : threshold * 5.0f) : 0.0f;
        
        Reset();
        StartSampler();
    }
};

//IntervalTimer MicrophoneFourierIT::sampleTimer;
TimeStep MicrophoneFourierIT::timeStep = TimeStep(60);

const uint16_t MicrophoneFourierIT::FFTSize;
const uint8_t MicrophoneFourierIT::OutputBins;
uint16_t MicrophoneFourierIT::sampleRate = 8000;
uint16_t MicrophoneFourierIT::samples = 0;
uint16_t MicrophoneFourierIT::samplesStorage = 0;
uint8_t MicrophoneFourierIT::pin = 11;
float MicrophoneFourierIT::minDB = 50.0f;
float MicrophoneFourierIT::maxDB = 120.0f;
float MicrophoneFourierIT::threshold = 400.0f;
float MicrophoneFourierIT::refreshRate = 60.0f;
bool MicrophoneFourierIT::samplesReady = false;
bool MicrophoneFourierIT::isInitialized = false;
DerivativeFilter MicrophoneFourierIT::peakFilterRate;

uint16_t MicrophoneFourierIT::frequencyBins[];
float MicrophoneFourierIT::SampleArray[];
int MicrophoneFourierIT::SampleArrayInt[];
float MicrophoneFourierIT::inputStorage[];
int MicrophoneFourierIT::inputStorageInt[];
float MicrophoneFourierIT::outputMagn[];
float MicrophoneFourierIT::outputData[];
float MicrophoneFourierIT::outputDataFilt[];
FFTFilter MicrophoneFourierIT::fftFilters[];

//arm_cfft_radix4_instance_f32 MicrophoneFourierIT::RadixFFT;

//hw_timer_t* MicrophoneFourierIT::timer = NULL;
//portMUX_TYPE MicrophoneFourierIT::timerMux = portMUX_INITIALIZER_UNLOCKED;
//const char* MicrophoneFourierIT::TAG = "main";
esp_timer_handle_t MicrophoneFourierIT::timer;
const esp_timer_create_args_t MicrophoneFourierIT::timerParameters = { .callback = reinterpret_cast<esp_timer_cb_t>(&SamplerCallback) };

