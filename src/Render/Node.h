#pragma once

#include "BoundingBox2D.h"
#include "Triangle2D.h"

class Node {
private:
    static const int maxEntities = 16; //The maximum number of entities in a leaf node before subdividing
    static constexpr float maxSubdivRatio = 0.5f;
    static const int maxDepth = 8;
    uint16_t count = 0;
    uint16_t capacity = 0;
    Node* childNodes = NULL;
    Triangle2D** entities = NULL;

    // Arena mode: when set, all allocations come from preallocated pools owned by Camera.
    // This eliminates per-frame heap fragmentation.
    bool mArenaMode = false;
    Node* mNodePool = nullptr;
    int* mNodePoolIdx = nullptr;
    int mNodePoolCap = 0;
    Triangle2D** mRefPool = nullptr;
    int* mRefPoolIdx = nullptr;
    int mRefPoolCap = 0;

public:
    Node() {};

    ~Node() {
        if (!mArenaMode) {
            if(entities)
                delete[] entities;
            if(childNodes)
                delete[] childNodes;
        }
    }

    // Enable arena mode: all node/ref allocations will bump from these preallocated pools.
    void UseArena(Node* nodePool, int* nodePoolIdx, int nodePoolCap,
                  Triangle2D** refPool, int* refPoolIdx, int refPoolCap) {
        mArenaMode = true;
        mNodePool = nodePool;
        mNodePoolIdx = nodePoolIdx;
        mNodePoolCap = nodePoolCap;
        mRefPool = refPool;
        mRefPoolIdx = refPoolIdx;
        mRefPoolCap = refPoolCap;
    }

    Node* GetChildNodes(){
        return childNodes;
    }

    Triangle2D** GetEntities(){
        return entities;
    }

    uint16_t GetCount(){
        return count;
    }

    void Expand(unsigned int newCount) {
        if (mArenaMode && mRefPool && mRefPoolIdx) {
            if (*mRefPoolIdx + (int)newCount > mRefPoolCap) return; // overflow — keep current
            Triangle2D** tmp = entities;
            entities = &mRefPool[*mRefPoolIdx];
            *mRefPoolIdx += (int)newCount;
            for (unsigned int i = 0; i < newCount; ++i) {
                entities[i] = (i < count && tmp) ? tmp[i] : NULL;
            }
            capacity = newCount;
            return;
        }
        // Heap fallback
        Triangle2D** tmp = entities;
        Triangle2D** newEnts = new Triangle2D*[newCount];
        if (!newEnts) return;
        entities = newEnts;
        for (unsigned int i = 0; i < newCount; ++i) {
            if (i < count && tmp)
                entities[i] = tmp[i];
            else
                entities[i] = NULL;
        }
        delete[] tmp;
        capacity = newCount;
    }

    //Note: node bboxes are implicit so we dont store them, therefore they need to be supplied externally whenever needed
    bool Insert(Triangle2D* triangle, BoundingBox2D& bbox, unsigned int depth = 0) {
        if (!triangle->DidIntersect(bbox)) {
            return false;
        }

        if (count == capacity)
            Expand(capacity? 2 * capacity : maxEntities);

        if (!entities) return false;
        entities[count] = triangle;
        ++count;

        return true;
    }

    void Subdivide(BoundingBox2D& bbox, unsigned int depth = 0) {
        if (depth == maxDepth)
            return;

        // Allocate 4 child nodes from arena or heap
        if (mArenaMode && mNodePool && mNodePoolIdx) {
            if (*mNodePoolIdx + 4 > mNodePoolCap) return; // overflow — keep as leaf
            childNodes = &mNodePool[*mNodePoolIdx];
            *mNodePoolIdx += 4;
            // Initialize arena nodes
            for (int i = 0; i < 4; ++i) {
                childNodes[i] = Node();
                childNodes[i].UseArena(mNodePool, mNodePoolIdx, mNodePoolCap,
                                       mRefPool, mRefPoolIdx, mRefPoolCap);
            }
        } else {
            childNodes = new Node[4];
            if (!childNodes) return;
        }

        Vector2D mid = (bbox.GetMinimum() + bbox.GetMaximum()) * 0.5f;
        BoundingBox2D bboxes[] = { {bbox.GetMinimum(), mid}, {{mid.X, bbox.GetMinimum().Y}, {bbox.GetMaximum().X, mid.Y} },
                                    {{bbox.GetMinimum().X, mid.Y}, {mid.X, bbox.GetMaximum().Y}}, {mid, bbox.GetMaximum()} };

        for (int j = 0; j < count; ++j) {
            int entityCount = 0;
            for (int i = 0; i < 4; ++i) {
                entityCount += childNodes[i].Insert(entities[j], bboxes[i], depth + 1);
            }
        }

        if (!mArenaMode) {
            delete[] entities;
        }
        entities = NULL;

        //edge case: stop subdividing if we cant improve the average entity counts (eg due to suboptimal mesh topology)
        float avgEntities = 0.0f;
        for (int i = 0; i < 4; ++i)
            avgEntities += 0.25f * (float)childNodes[i].count;

        bool canSubdiv = avgEntities < maxSubdivRatio * (float)count;

        if (!canSubdiv) {
            count = 0;
            return;
        }

        count = 0;

        for (int i = 0; i < 4; ++i) {
            if(childNodes[i].count > maxEntities)
                childNodes[i].Subdivide(bboxes[i], depth + 1);
        }
    }

    bool IsLeaf() {
        return !childNodes;
    }

    /*
    void PrintStats(int& totalCount, BoundingBox2D& bbox) {
        if (IsLeaf()) {
            //printf("count: %d\n", count);
            totalCount += count;
        }
        else {
            Vector2D mid = (bbox.GetMinimum() + bbox.GetMaximum()) * 0.5f;
            BoundingBox2D bboxes[] = { {bbox.GetMinimum(), mid}, {{mid.X, bbox.GetMinimum().Y}, {bbox.GetMaximum().X, mid.Y} },
                                {{bbox.GetMinimum().X, mid.Y}, {mid.X, bbox.GetMaximum().Y}}, {mid, bbox.GetMaximum()} };
            for (int i = 0; i < 4; ++i) {
                childNodes[i].PrintStats(totalCount, bboxes[i]);
            }
        }
    }
    */
};