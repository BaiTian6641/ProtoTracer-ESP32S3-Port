#pragma once

#include "../Screenspace/Effect.h"
#include "Object3D.h"
#include <esp_heap_caps.h> // Required for PSRAM allocation
#include <cstdint>          // Required for uint8_t

class Scene {
private:
    const int maxObjects;
    Object3D** objects; // This will point to PSRAM (aligned for faster bursts)
    Object3D** objectsShadow = nullptr; // Optional internal-RAM mirror for iteration speed
    bool shadowOwned = false; // track whether shadow was allocated
    unsigned int numObjects = 0;
    Effect* effect;
    bool doesUseEffect = false;

public:
    // Constructor: Allocate the 'objects' array in PSRAM
    Scene(unsigned int maxObjects) : maxObjects(maxObjects) {
        // Align to 16 bytes and allow DMA to improve burst reads from PSRAM.
        objects = (Object3D**)heap_caps_aligned_alloc(16, maxObjects * sizeof(Object3D*),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

        // ALWAYS check if allocation succeeded.
        if (objects == nullptr) {
            // Fallback to a non-aligned PSRAM block.
            objects = (Object3D**)heap_caps_malloc(maxObjects * sizeof(Object3D*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        if (objects == nullptr) {
            // Final fallback to internal heap to avoid null deref crashes; may fail if not enough RAM.
            objects = (Object3D**)heap_caps_malloc(maxObjects * sizeof(Object3D*), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        }

        // Optional internal shadow to keep iteration off PSRAM; falls back silently if not available.
        objectsShadow = (Object3D**)heap_caps_malloc(maxObjects * sizeof(Object3D*), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        shadowOwned = objectsShadow != nullptr;
    }

    // Destructor: Free the PSRAM-allocated memory
    ~Scene(){
        // Use heap_caps_free for memory allocated with heap_caps_malloc.
        heap_caps_free(objects);
        if (shadowOwned) {
            heap_caps_free(objectsShadow);
        }
    }

    // --- AddObject remains the same ---
    void AddObject(Object3D* object){
        if (!objects) {
            return; // allocation failed; avoid crash
        }

        if (numObjects < maxObjects) {
            objects[numObjects] = object;
            if (objectsShadow) objectsShadow[numObjects] = object;
            numObjects++;
        } else {
            // Cache full: free shadow to reclaim internal RAM; PSRAM backing remains
            if (shadowOwned) {
                heap_caps_free(objectsShadow);
                objectsShadow = nullptr;
                shadowOwned = false;
            }
        }
    }

    // --- REFACTORED & SIMPLIFIED REMOVAL LOGIC ---

    // Remove object by its index in the array
    void RemoveObject(unsigned int index){
        if (index >= numObjects) {
            return; // Invalid index, do nothing
        }
        
        // Shift all subsequent elements one position to the left
        for(unsigned int i = index; i < numObjects - 1; i++){
            objects[i] = objects[i + 1];
            if (objectsShadow) objectsShadow[i] = objectsShadow[i + 1];
        }

        numObjects--; // Decrement the count of objects

        // If many objects were removed and a shadow exists, consider freeing the shadow to reclaim internal RAM
        if (shadowOwned && numObjects == 0) {
            heap_caps_free(objectsShadow);
            objectsShadow = nullptr;
            shadowOwned = false;
        }
    }

    // Remove object by its pointer
    void RemoveObject(Object3D* object){
        for(unsigned int i = 0; i < numObjects; i++){
            if(objects[i] == object){
                RemoveObject(i); // Call the index-based removal function
                return;          // Exit after removing the first match
            }
        }
    }

    // --- Other methods remain the same ---

    bool UseEffect(){
        return doesUseEffect;
    }

    void EnableEffect(){
        doesUseEffect = true;
    }

    void DisableEffect(){
        doesUseEffect = false;
    }
    
    Effect* GetEffect(){
        return effect;
    }

    void SetEffect(Effect* effect){
        this->effect = effect;
    }

    Object3D** GetObjects(){
        // Prefer shadow in internal RAM if available
        return objectsShadow ? objectsShadow : objects;
    }

    // Cached accessors to use the internal-RAM shadow when present
    Object3D** GetCachedObjects(){
        return objectsShadow ? objectsShadow : objects;
    }

    unsigned int GetCachedObjectCount() const {
        return numObjects;
    }

    // Changed return type to match numObjects type for consistency
    unsigned int GetObjectCount(){
        return numObjects;
    }
};