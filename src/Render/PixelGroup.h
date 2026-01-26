#pragma once

#include "IPixelGroup.h"
#include <esp_heap_caps.h>

// Define a sentinel value to indicate no neighbor exists.
#define NO_NEIGHBOR (static_cast<unsigned int>(-1))

class PixelGroup : public IPixelGroup{
private:
    // --- MODIFIED: pixelCount is now a member variable, not a template parameter ---
    const size_t pixelCount;

    Direction direction;
    BoundingBox2D bounds;
	const Vector2D* pixelPositions;

    // --- MODIFIED: All large arrays are now pointers ---
  	ProtoRGBColor* pixelColors;
  	ProtoRGBColor* pixelBuffer;
    unsigned int* up;
    unsigned int* down;
    unsigned int* left;
    unsigned int* right;

    Vector2D* rectCoords = nullptr; // precomputed coordinates for rectangular layouts
    bool rectCoordsBuilt = false;

    // --- REMOVED: The redundant boolean arrays are gone ---
    // bool upExists[pixelCount]; ... etc.

    bool isRectangular = false;
    uint16_t rowCount;
    uint16_t colCount;
    Vector2D size;
    Vector2D position;

    //Allocate all array on external PSRAM
    void AllocateMemory() {
        // Try to keep hot color buffers in internal RAM for steadier bandwidth; fall back to PSRAM/new on failure.
        pixelColors = static_cast<ProtoRGBColor*>(heap_caps_malloc(pixelCount * sizeof(ProtoRGBColor), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        pixelBuffer = static_cast<ProtoRGBColor*>(heap_caps_malloc(pixelCount * sizeof(ProtoRGBColor), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        if (!pixelColors) pixelColors = new ProtoRGBColor[pixelCount];
        if (!pixelBuffer) pixelBuffer = new ProtoRGBColor[pixelCount];

        up = new unsigned int[pixelCount];
        down = new unsigned int[pixelCount];
        left = new unsigned int[pixelCount];
        right = new unsigned int[pixelCount];
    }

    //Deallocate those arrays
    void DeallocateMemory() {
        heap_caps_free(pixelColors);
        heap_caps_free(pixelBuffer);
        delete[] up;
        delete[] down;
        delete[] left;
        delete[] right;
        delete[] rectCoords;
    }

public:
    PixelGroup(size_t pCount, Vector2D size, Vector2D position, uint16_t rowCount) : pixelCount(pCount) {
        AllocateMemory(); // Allocate memory in PSRAM

        this->size = size;
        this->position = position;
        this->rowCount = rowCount;
        this->colCount = pixelCount / rowCount;

        isRectangular = true;

        rectCoords = new Vector2D[pixelCount];

        bounds.UpdateBounds(position - (size / 2.0f));
        bounds.UpdateBounds(position + (size / 2.0f));

        for(unsigned int i = 0; i < pixelCount; i++){
            pixelColors[i] = ProtoRGBColor();
            pixelBuffer[i] = ProtoRGBColor();

            // Precompute coordinate for this rectangular pixel
            float row = i % rowCount;
            float col = (i - row) / rowCount;
            rectCoords[i].X = Mathematics::Map(row, 0.0f, float(rowCount), position.X - size.X / 2.0f, position.X + size.X / 2.0f);
            rectCoords[i].Y = Mathematics::Map(col, 0.0f, float(colCount), position.Y - size.Y / 2.0f, position.Y + size.Y / 2.0f);
        }

        rectCoordsBuilt = true;
    }

    PixelGroup(size_t pCount, const Vector2D* pixelLocations, Direction direction = ZEROTOMAX) : pixelCount(pCount) {
        AllocateMemory(); // Allocate memory in PSRAM

        this->direction = direction;
        pixelPositions = pixelLocations;

        for(unsigned int i = 0; i < pixelCount; i++){
            bounds.UpdateBounds(pixelLocations[i]);
        }

        GridSort();
        //ListPixelNeighbors();
    }

    ~PixelGroup() {
        DeallocateMemory();
    }

    Vector2D GetCoordinate(unsigned int count) override {
        count = Mathematics::Constrain<int>(count, 0, pixelCount);

        if (isRectangular){
            return rectCoordsBuilt ? rectCoords[count] : Vector2D();
        }
        else{
            if(direction == ZEROTOMAX){
                return pixelPositions[count];
            }
            else{
                return pixelPositions[pixelCount - count - 1];
            }
        }
    }

    int GetPixelIndex(Vector2D location) override {
        float row = Mathematics::Map(location.X, position.X - size.X / 2.0f, position.X + size.X / 2.0f, 0.0f, float(rowCount));
        float col = Mathematics::Map(location.Y, position.Y - size.Y / 2.0f, position.Y + size.Y / 2.0f, 0.0f, float(colCount));

        unsigned int count = row + col * rowCount;

        if (count < pixelCount && count > 0 && row > 0 && row < rowCount && col > 0 && col < colCount){
            return count;
        }
        else{
            return -1;
        }
    }

    ProtoRGBColor* GetColor(unsigned int count) override {
        return &pixelColors[count];
    }

    ProtoRGBColor* GetColors() override {
        return &pixelColors[0];
    }

    ProtoRGBColor* GetColorBuffer() override {
        return &pixelBuffer[0];
    }

    unsigned int GetPixelCount() override {
        return pixelCount;
    }

    bool Overlaps(BoundingBox2D* box) override {
        return bounds.Overlaps(box);
    }

    bool ContainsVector2D(Vector2D v) override {
        return v.CheckBounds(bounds.GetMinimum(), bounds.GetMaximum());
    }

    // ... The rest of your public methods (GetCoordinate, GetPixelIndex, etc.) remain unchanged ...
    // ... EXCEPT for the neighbor-finding methods, which need to be updated.           ...

    // --- MODIFIED: Neighbor-finding methods now check for the sentinel value ---

    bool GetUpIndex(unsigned int count, unsigned int* upIndex) override {
        if (isRectangular){
            unsigned int index = count + rowCount;
            if (index < pixelCount){
                *upIndex = index;
                return true;
            }
            return false;
        }
        else {
            if (up[count] != NO_NEIGHBOR) {
                *upIndex = up[count];
                return true;
            }
            return false;
        }
    }

    bool GetDownIndex(unsigned int count, unsigned int* downIndex) override {
        if (isRectangular){
            long index = static_cast<long>(count) - rowCount; // Use long for check
            if (index >= 0){
                *downIndex = static_cast<unsigned int>(index);
                return true;
            }
            return false;
        }
        else {
            if (down[count] != NO_NEIGHBOR) {
                *downIndex = down[count];
                return true;
            }
            return false;
        }
    }

    bool GetLeftIndex(unsigned int count, unsigned int* leftIndex) override {
        if (isRectangular){
            if (count > 0 && (count % rowCount != 0)) {
                *leftIndex = count - 1;
                return true;
            }
            return false;
        }
        else {
            if (left[count] != NO_NEIGHBOR) {
                *leftIndex = left[count];
                return true;
            }
            return false;
        }
    }

    bool GetRightIndex(unsigned int count, unsigned int* rightIndex) override {
        if (isRectangular){
            if ((count + 1) < pixelCount && ((count + 1) % rowCount != 0)) {
                *rightIndex = count + 1;
                return true;
            }
            return false;
        }
        else {
            if (right[count] != NO_NEIGHBOR) {
                *rightIndex = right[count];
                return true;
            }
            return false;
        }
    }

    bool GetAlternateXIndex(unsigned int count, unsigned int* index, int pixels){
        unsigned int tempIndex = count;
        bool isEven = count % 2;
        bool valid = true;

        for(unsigned int i = 0; i < count / 2; i++){
            if (isEven){
                valid = GetRightIndex(tempIndex, &tempIndex);
            }
            else{
                valid = GetLeftIndex(tempIndex, &tempIndex);
            }
            
            if (!valid) break;
        }

        *index = tempIndex;

        return valid;
    }

    bool GetAlternateYIndex(unsigned int count, unsigned int* index, int pixels){
        unsigned int tempIndex = count;
        bool isEven = count % 2;
        bool valid = true;

        for(unsigned int i = 0; i < count / 2; i++){
            if (isEven){
                valid = GetUpIndex(tempIndex, &tempIndex);
            }
            else{
                valid = GetDownIndex(tempIndex, &tempIndex);
            }
            
            if (!valid) break;
        }
        
        *index = tempIndex;

        return valid;
    }

    bool GetOffsetXYIndex(unsigned int count, unsigned int* index, int x1, int y1) override {
        unsigned int tempIndex = count;
        bool valid = true;

        for(int i = 0; i < x1; i++){
            if (x1 > 0) valid = GetRightIndex(tempIndex, &tempIndex);
            else if (x1 < 0) valid = GetLeftIndex(tempIndex, &tempIndex);
            else break;
            
            if (!valid) break;
        }
        
        for(int i = 0; i < y1; i++){
            if (y1 > 0) valid = GetUpIndex(tempIndex, &tempIndex);
            else if (y1 < 0) valid = GetDownIndex(tempIndex, &tempIndex);
            else break;
            
            if (!valid) break;
        }
        
        *index = tempIndex;

        return valid;
    }

    bool GetRadialIndex(unsigned int count, unsigned int* index, int pixels, float angle) override {//walks in the direction of the angle to a target pixel to grab an index
        int x1 = int(float(pixels) * cosf(angle * Mathematics::MPID180));
        int y1 = int(float(pixels) * sinf(angle * Mathematics::MPID180));

        unsigned int tempIndex = count;
        unsigned int tTempIndex = 0;
        bool valid = true;

        int previousX = 0;
        int previousY = 0;

        int x = 0;
        int y = 0;

        for(int i = 0; i < pixels; i++){
            x = Mathematics::Map(i, 0, pixels, 0, x1);
            y = Mathematics::Map(i, 0, pixels, 0, y1);

            if (x > previousX) valid = GetRightIndex(tempIndex, &tTempIndex);
            else if (x < previousX) valid = GetLeftIndex(tempIndex, &tTempIndex);

            tempIndex = tTempIndex;

            if (y > previousY) valid = GetUpIndex(tempIndex, &tTempIndex);
            else if (y < previousY) valid = GetDownIndex(tempIndex, &tTempIndex);

            tempIndex = tTempIndex;
        
            if (!valid) break;

            previousX = x;
            previousY = y;
        }

        *index = tempIndex;

        return valid;
    }

    // ... Other methods like GetAlternateXIndex, GetRadialIndex, etc. will work as-is ...
    // ... because they rely on the Get...Index methods we just updated.            ...

    void GridSort() override {
        if(!isRectangular){
            // --- MODIFIED: Initialize all neighbor indices to the sentinel value first ---
            for (unsigned int i = 0; i < pixelCount; i++) {
                up[i] = NO_NEIGHBOR;
                down[i] = NO_NEIGHBOR;
                left[i] = NO_NEIGHBOR;
                right[i] = NO_NEIGHBOR;
            }

            // Loop through all pixels
            for (unsigned int i = 0; i < pixelCount; i++) {
                Vector2D currentPos = pixelPositions[i];

                float minUp = Mathematics::FLTMAX, minDown = Mathematics::FLTMAX, minLeft = Mathematics::FLTMAX, minRight = Mathematics::FLTMAX;
                int minUpIndex = -1, minDownIndex = -1, minLeftIndex = -1, minRightIndex = -1;

                // Loop through all other pixels
                for (unsigned int j = 0; j < pixelCount; j++) {
                    if (i == j) { continue; } 

                    Vector2D neighborPos = pixelPositions[j];
                    float dist = currentPos.CalculateEuclideanDistance(neighborPos);

                    if (Mathematics::IsClose(currentPos.X, neighborPos.X, 1.0f)) {
                        if (currentPos.Y < neighborPos.Y && dist < minUp) {
                            minUp = dist;
                            minUpIndex = j;
                        }
                        else if (currentPos.Y > neighborPos.Y && dist < minDown) {
                            minDown = dist;
                            minDownIndex = j;
                        }
                    }

                    if (Mathematics::IsClose(currentPos.Y, neighborPos.Y, 1.0f)) {
                        if (currentPos.X > neighborPos.X && dist < minLeft) {
                            minLeft = dist;
                            minLeftIndex = j;
                        }
                        else if (currentPos.X < neighborPos.X && dist < minRight) {
                            minRight = dist;
                            minRightIndex = j;
                        }
                    }
                }

                // --- MODIFIED: Set the indices directly, no more boolean flags ---
                if (minUpIndex != -1)   up[i] = minUpIndex;
                if (minDownIndex != -1) down[i] = minDownIndex;
                if (minLeftIndex != -1) left[i] = minLeftIndex;
                if (minRightIndex != -1) right[i] = minRightIndex;
            }
        }
    }

    BoundingBox2D& GetBounds() {
        return bounds;
    }

    // ... ListPixelNeighbors needs a small update to print the sentinel ...
    //But we don't need use that for now, for now?

    //void ListPixelNeighbors(){
    //    for(unsigned int i = 0; i < pixelCount; i++){
    //        Serial.print(i); Serial.print('\t');
    //        Serial.print(up[i]); Serial.print('\t');
    //        Serial.print(down[i]); Serial.print('\t');
    //        Serial.print(left[i]); Serial.print('\t');
    //        Serial.print(right[i]); Serial.print('\t');
    //        Serial.print(upExists[i]); Serial.print('\t');
    //        Serial.print(downExists[i]); Serial.print('\t');
    //        Serial.print(leftExists[i]); Serial.print('\t');
    //        Serial.print(rightExists[i]); Serial.print('\n');
    //    }
    //}
};
