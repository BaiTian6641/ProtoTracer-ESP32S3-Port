#pragma once

#include "Triangle3D.h"
#include "IndexGroup.h"
#include <cstddef>

class TriangleGroup{
private:
    Vector3D* vertices;
    Triangle3D* triangles;
    IndexGroup* indexGroup;
    IndexGroup* uvIndexGroup;
    Vector2D* uvVertices;
    int vertexCount;
    int triangleCount;
    int uvVertexCount;
    bool hasUV = false;
    bool ownsVertices = true;
    bool ownsIndexGroup = true;

public:
    TriangleGroup(Vector3D* vertices, IndexGroup* indexGroup, int vertexCount, int triangleCount) : vertexCount(vertexCount), triangleCount(triangleCount){
        this->vertices = vertices;
        this->indexGroup = indexGroup;

        triangles = new Triangle3D[triangleCount];
        
        for (int i = 0; i < triangleCount; i++) {
            triangles[i].p1 = &vertices[indexGroup[i].A];
            triangles[i].p2 = &vertices[indexGroup[i].B];
            triangles[i].p3 = &vertices[indexGroup[i].C];
        }
    }

    // External-vertex constructor: caller owns the vertex buffer and index/UV data.
    // Triangle pointers are wired to externalVertices using the same offsets as source.
    TriangleGroup(Vector3D* externalVertices, TriangleGroup* source)
        : vertexCount(source->GetVertexCount()), triangleCount(source->GetTriangleCount()) {
        vertices = externalVertices;
        ownsVertices = false;
        indexGroup = source->GetIndexGroup();
        ownsIndexGroup = false;

        if (source->hasUV) {
            hasUV = true;
            uvVertexCount = source->uvVertexCount;
            uvIndexGroup = source->uvIndexGroup;
            uvVertices = source->uvVertices;
        }

        triangles = new Triangle3D[triangleCount];
        Triangle3D* srcTris = source->GetTriangles();
        Vector3D* srcVerts = source->GetVertices();
        for (int i = 0; i < triangleCount; i++) {
            ptrdiff_t off1 = srcTris[i].p1 - srcVerts;
            ptrdiff_t off2 = srcTris[i].p2 - srcVerts;
            ptrdiff_t off3 = srcTris[i].p3 - srcVerts;
            triangles[i].p1 = &vertices[off1];
            triangles[i].p2 = &vertices[off2];
            triangles[i].p3 = &vertices[off3];
            if (hasUV) {
                triangles[i].p1UV = srcTris[i].p1UV;
                triangles[i].p2UV = srcTris[i].p2UV;
                triangles[i].p3UV = srcTris[i].p3UV;
                triangles[i].hasUV = true;
            }
        }
    }

    TriangleGroup(Vector3D* vertices, IndexGroup* indexGroup, IndexGroup* uvIndexGroup, Vector2D* uvVertices, int vertexCount, int triangleCount, int uvVertexCount) : vertexCount(vertexCount), triangleCount(triangleCount), uvVertexCount(uvVertexCount){
        this->vertices = vertices;
        this->indexGroup = indexGroup;
        this->uvIndexGroup = uvIndexGroup;
        this->uvVertices = uvVertices;
        this->hasUV = true;

        triangles = new Triangle3D[triangleCount];
        
        for (int i = 0; i < triangleCount; i++) {
            triangles[i].p1 = &vertices[indexGroup[i].A];
            triangles[i].p2 = &vertices[indexGroup[i].B];
            triangles[i].p3 = &vertices[indexGroup[i].C];
            triangles[i].p1UV = &uvVertices[uvIndexGroup[i].A];
            triangles[i].p2UV = &uvVertices[uvIndexGroup[i].B];
            triangles[i].p3UV = &uvVertices[uvIndexGroup[i].C];

            triangles[i].hasUV = true;
        }
    }

    TriangleGroup(TriangleGroup* triangleGroup) : vertexCount(triangleGroup->GetVertexCount()), triangleCount(triangleGroup->GetTriangleCount()){
        indexGroup = triangleGroup->GetIndexGroup();//use existing reference, will not change
        ownsIndexGroup = false;//borrowed: the source owns and frees indexGroup; copying it
        //               and also freeing here would double-free (latent bug — masked today
        //               only because render objects are never destroyed).
        vertices = new Vector3D[vertexCount];//copy to new array
        triangles = new Triangle3D[triangleCount];//copy to new array

        if(triangleGroup->hasUV){
            this->hasUV = triangleGroup->hasUV;

            uvVertices = triangleGroup->uvVertices;
            uvIndexGroup = triangleGroup->uvIndexGroup;
        }

        for (int i = 0; i < vertexCount; i++){
            vertices[i] = Vector3D(triangleGroup->GetVertices()[i]);
        }

        for (int i = 0; i < triangleCount; i++) {
            triangles[i].p1 = &vertices[indexGroup[i].A];
            triangles[i].p2 = &vertices[indexGroup[i].B];
            triangles[i].p3 = &vertices[indexGroup[i].C];

            if(triangleGroup->hasUV){
                triangles[i].p1UV = &uvVertices[uvIndexGroup[i].A];
                triangles[i].p2UV = &uvVertices[uvIndexGroup[i].B];
                triangles[i].p3UV = &uvVertices[uvIndexGroup[i].C];

                triangles[i].hasUV = true;
            }
        }

    }

    TriangleGroup(TriangleGroup** triangleGroups, const int triangleGroupCount){
        vertexCount = 0;
        triangleCount = 0;

        for (int i = 0; i < triangleGroupCount; i++){
            vertexCount += triangleGroups[i]->GetVertexCount();
            triangleCount += triangleGroups[i]->GetTriangleCount();
        }

        indexGroup = new IndexGroup[triangleCount];//use existing reference, will not change
        vertices = new Vector3D[vertexCount];//copy to new array
        triangles = new Triangle3D[triangleCount];//copy to new array

        int vCounter = 0, tCounter = 0, vOffset = 0;
        
        for(int i = 0; i < triangleGroupCount; i++){
            for(int j = 0; j < triangleGroups[i]->GetVertexCount(); j++){
                vertices[vCounter] = Vector3D(triangleGroups[i]->GetVertices()[j]);
                vCounter++;
            }

            for(int j = 0; j < triangleGroups[i]->GetTriangleCount(); j++){
                triangles[tCounter] = Triangle3D();
                indexGroup[tCounter] = IndexGroup(triangleGroups[i]->GetIndexGroup()[j]).Add(IndexGroup(vOffset, vOffset, vOffset));
                
                triangles[tCounter].p1 = &vertices[indexGroup[tCounter].A];
                triangles[tCounter].p2 = &vertices[indexGroup[tCounter].B];
                triangles[tCounter].p3 = &vertices[indexGroup[tCounter].C];

                tCounter++;
            }
            
            vOffset = vOffset + triangleGroups[i]->GetVertexCount();
        }
    }

    ~TriangleGroup(){
        if (ownsVertices) delete[] vertices;
        delete[] triangles;
        if (ownsIndexGroup) delete[] indexGroup;
    }

    IndexGroup* GetIndexGroup(){
        return indexGroup;
    }

    int GetTriangleCount(){
        return triangleCount;
    }

    Vector3D* GetVertices(){
        return vertices;
    }

    int GetVertexCount(){
        return vertexCount;
    }

    Triangle3D* GetTriangles(){
        return triangles;
    }

    Vector2D* GetUVVertices(){
        return uvVertices;
    }

};
