#pragma once

#include "Node.h"

class QuadTree {
private:
    static const int maxEntities = 16; //The maximum number of entities in a leaf node before subdividing
    Triangle2D* entities = NULL;
    BoundingBox2D bbox;
    Node root;
    int count = 0;
    int capacity = 0;

public:
    QuadTree(const BoundingBox2D& bounds, int reserveCapacity = 0): bbox(bounds){
        if (reserveCapacity > 0) {
            Expand(reserveCapacity);
        }
    }

    ~QuadTree() {
        free(entities);
    }

    bool Insert(Triangle2D* triangle){
        bool inserted = root.Insert(triangle, bbox);
        if (inserted)
            ++count;
        return inserted;
    }

    void Expand(int newCapacity) {
        entities = (Triangle2D*)realloc(entities, newCapacity * sizeof(Triangle2D));
        capacity = newCapacity;
    }

    void Reserve(int reserveCapacity) {
        if (reserveCapacity > capacity) {
            Expand(reserveCapacity);
        }
    }

    bool Insert(Triangle2D triangle) {
        if (count == capacity)
            Expand(capacity ? (1.5f * capacity) : maxEntities);

        entities[count] = triangle;

        bool inserted = root.Insert(&entities[count], bbox);

        if (inserted)
            ++count;

        return inserted;
    }

    Node* Intersect(const Vector2D& p) {
        if (!bbox.Contains(p)) {
            return NULL;
        }

        Node* currentNode = &root;
        BoundingBox2D currentBbox = bbox;

        while (true) {
            if (currentNode->IsLeaf())
                return currentNode;

            Vector2D mid = {(currentBbox.GetMinimum().X + currentBbox.GetMaximum().X) * 0.5f, (currentBbox.GetMinimum().Y + currentBbox.GetMaximum().Y) * 0.5f};

            const bool right = p.X >= mid.X;
            const bool top = p.Y >= mid.Y;
            const int childIndex = (right ? 1 : 0) + (top ? 2 : 0);

            currentNode = &(currentNode->GetChildNodes()[childIndex]);

            if (!right && !top) {
                currentBbox = BoundingBox2D(currentBbox.GetMinimum(), mid);
            } else if (right && !top) {
                currentBbox = BoundingBox2D(Vector2D(mid.X, currentBbox.GetMinimum().Y), Vector2D(currentBbox.GetMaximum().X, mid.Y));
            } else if (!right && top) {
                currentBbox = BoundingBox2D(Vector2D(currentBbox.GetMinimum().X, mid.Y), Vector2D(mid.X, currentBbox.GetMaximum().Y));
            } else {
                currentBbox = BoundingBox2D(mid, currentBbox.GetMaximum());
            }
        }
    }

    void Rebuild() {
        for (int i = 0; i < count; ++i)
            root.GetEntities()[i] = &entities[i];

        root.Subdivide(bbox);
    }

    /*
    void PrintStats() {
        int totalCount = 0;
        printf("total inserts: %d\n", count);
        printf("node stats: \n");
        root.PrintStats(totalCount, bbox);
        printf("total entities: %d \n", totalCount);
    }
    */

};
