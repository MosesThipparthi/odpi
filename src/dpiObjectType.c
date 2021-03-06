//-----------------------------------------------------------------------------
// Copyright (c) 2016, 2017 Oracle and/or its affiliates.  All rights reserved.
// This program is free software: you can modify it and/or redistribute it
// under the terms of:
//
// (i)  the Universal Permissive License v 1.0 or at your option, any
//      later version (http://oss.oracle.com/licenses/upl); and/or
//
// (ii) the Apache License v 2.0. (http://www.apache.org/licenses/LICENSE-2.0)
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// dpiObjectType.c
//   Implementation of object types.
//-----------------------------------------------------------------------------

#include "dpiImpl.h"

// forward declarations of internal functions only used in this file
static int dpiObjectType__init(dpiObjectType *objType, void *param,
        uint32_t nameAttribute, dpiError *error);


//-----------------------------------------------------------------------------
// dpiObjectType__allocate() [INTERNAL]
//   Allocate and initialize an object type structure.
//-----------------------------------------------------------------------------
int dpiObjectType__allocate(dpiConn *conn, void *param,
        uint32_t nameAttribute, dpiObjectType **objType, dpiError *error)
{
    dpiObjectType *tempObjType;

    // create structure and retain reference to connection
    *objType = NULL;
    if (dpiGen__allocate(DPI_HTYPE_OBJECT_TYPE, conn->env,
            (void**) &tempObjType, error) < 0)
        return DPI_FAILURE;
    if (dpiGen__setRefCount(conn, error, 1) < 0) {
        dpiObjectType__free(tempObjType, error);
        return DPI_FAILURE;
    }
    tempObjType->conn = conn;

    // perform initialization
    if (dpiObjectType__init(tempObjType, param, nameAttribute, error) < 0) {
        dpiObjectType__free(tempObjType, error);
        return DPI_FAILURE;
    }

    *objType = tempObjType;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType__describe() [INTERNAL]
//   Describe the object type and store information about it. Note that a
// separate call to OCIDescribeAny() is made in order to support nested types;
// an illegal attribute value is returned if this is not done.
//-----------------------------------------------------------------------------
static int dpiObjectType__describe(dpiObjectType *objType,
        void *describeHandle, dpiError *error)
{
    void *collectionParam, *param;
    uint16_t typeCode;

    // describe the type
    if (dpiOci__describeAny(objType->conn, objType->tdo, 0, DPI_OCI_OTYPE_PTR,
            describeHandle, error) < 0)
        return DPI_FAILURE;

    // get top level parameter descriptor
    if (dpiOci__attrGet(describeHandle, DPI_OCI_HTYPE_DESCRIBE, &param, 0,
            DPI_OCI_ATTR_PARAM, "get top level parameter", error) < 0)
        return DPI_FAILURE;

    // determine type code
    if (dpiOci__attrGet(param, DPI_OCI_DTYPE_PARAM, &typeCode, 0,
            DPI_OCI_ATTR_TYPECODE, "get type code", error) < 0)
        return DPI_FAILURE;
    objType->typeCode = typeCode;

    // determine the number of attributes
    if (dpiOci__attrGet(param, DPI_OCI_DTYPE_PARAM,
            (void*) &objType->numAttributes, 0, DPI_OCI_ATTR_NUM_TYPE_ATTRS,
            "get number of attributes", error) < 0)
        return DPI_FAILURE;

    // if a collection, need to determine the element type
    if (typeCode == DPI_SQLT_NCO) {
        objType->isCollection = 1;

        // acquire collection parameter descriptor
        if (dpiOci__attrGet(param, DPI_OCI_DTYPE_PARAM, &collectionParam, 0,
                DPI_OCI_ATTR_COLLECTION_ELEMENT, "get collection descriptor",
                error) < 0)
            return DPI_FAILURE;

        // determine type of element
        if (dpiOracleType__populateTypeInfo(objType->conn, collectionParam,
                DPI_OCI_DTYPE_PARAM, &objType->elementTypeInfo, error) < 0)
            return DPI_FAILURE;

    }

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType__free() [INTERNAL]
//   Free the memory for an object type.
//-----------------------------------------------------------------------------
void dpiObjectType__free(dpiObjectType *objType, dpiError *error)
{
    if (objType->conn) {
        dpiGen__setRefCount(objType->conn, error, -1);
        objType->conn = NULL;
    }
    if (objType->elementTypeInfo.objectType) {
        dpiGen__setRefCount(objType->elementTypeInfo.objectType, error, -1);
        objType->elementTypeInfo.objectType = NULL;
    }
    if (objType->schema) {
        free((void*) objType->schema);
        objType->schema = NULL;
    }
    if (objType->name) {
        free((void*) objType->name);
        objType->name = NULL;
    }
    free(objType);
}


//-----------------------------------------------------------------------------
// dpiObjectType__init() [INTERNAL]
//   Initialize the object type.
//-----------------------------------------------------------------------------
static int dpiObjectType__init(dpiObjectType *objType, void *param,
        uint32_t nameAttribute, dpiError *error)
{
    void *describeHandle;
    void *tdoReference;

    // determine the schema of the type
    if (dpiUtils__getAttrStringWithDup("get schema", param,
            DPI_OCI_DTYPE_PARAM, DPI_OCI_ATTR_SCHEMA_NAME, &objType->schema,
            &objType->schemaLength, error) < 0)
        return DPI_FAILURE;

    // determine the name of the type
    if (dpiUtils__getAttrStringWithDup("get name", param, DPI_OCI_DTYPE_PARAM,
            nameAttribute, &objType->name, &objType->nameLength, error) < 0)
        return DPI_FAILURE;

    // retrieve TDO of the parameter and pin it in the cache
    if (dpiOci__attrGet(param, DPI_OCI_DTYPE_PARAM, (void*) &tdoReference, 0,
            DPI_OCI_ATTR_REF_TDO, "get TDO reference", error) < 0)
        return DPI_FAILURE;
    if (dpiOci__objectPin(objType->env, tdoReference, &objType->tdo,
            error) < 0)
        return DPI_FAILURE;

    // acquire a describe handle
    if (dpiOci__handleAlloc(objType->env, &describeHandle,
            DPI_OCI_HTYPE_DESCRIBE, "allocate describe handle", error) < 0)
        return DPI_FAILURE;

    // describe the type
    if (dpiObjectType__describe(objType, describeHandle, error) < 0) {
        dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
        return DPI_FAILURE;
    }

    // free the describe handle
    dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType_addRef() [PUBLIC]
//   Add a reference to the object type.
//-----------------------------------------------------------------------------
int dpiObjectType_addRef(dpiObjectType *objType)
{
    return dpiGen__addRef(objType, DPI_HTYPE_OBJECT_TYPE, __func__);
}


//-----------------------------------------------------------------------------
// dpiObjectType_createObject() [PUBLIC]
//   Create a new object of the specified type and return it. Return NULL on
// error.
//-----------------------------------------------------------------------------
int dpiObjectType_createObject(dpiObjectType *objType, dpiObject **obj)
{
    dpiObject *tempObj;
    dpiError error;

    // validate parameters
    if (dpiGen__startPublicFn(objType, DPI_HTYPE_OBJECT_TYPE, __func__,
            &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(obj)

    // create the object
    if (dpiObject__allocate(objType, NULL, NULL, 0, &tempObj, &error) < 0)
        return DPI_FAILURE;

    // create the object instance data
    if (dpiOci__objectNew(tempObj, &error) < 0) {
        dpiGen__setRefCount(tempObj, &error, -1);
        return DPI_FAILURE;
    }

    // get the null indicator structure
    if (dpiOci__objectGetInd(tempObj, &error) < 0) {
        dpiGen__setRefCount(tempObj, &error, -1);
        return DPI_FAILURE;
    }

    *obj = tempObj;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType_getAttributes() [PUBLIC]
//   Get the attributes for the object type in the provided array.
//-----------------------------------------------------------------------------
int dpiObjectType_getAttributes(dpiObjectType *objType, uint16_t numAttributes,
        dpiObjectAttr **attributes)
{
    void *topLevelParam, *attrListParam, *attrParam, *describeHandle;
    dpiError error;
    uint16_t i;

    // validate object type and the number of attributes
    if (dpiGen__startPublicFn(objType, DPI_HTYPE_OBJECT_TYPE, __func__,
            &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(attributes)
    if (numAttributes < objType->numAttributes)
        return dpiError__set(&error, "get attributes",
                DPI_ERR_ARRAY_SIZE_TOO_SMALL, numAttributes);
    if (numAttributes == 0)
        return DPI_SUCCESS;

    // acquire a describe handle
    if (dpiOci__handleAlloc(objType->env, &describeHandle,
            DPI_OCI_HTYPE_DESCRIBE, "allocate describe handle", &error) < 0)
        return DPI_FAILURE;

    // describe the type
    if (dpiOci__describeAny(objType->conn, objType->tdo, 0, DPI_OCI_OTYPE_PTR,
            describeHandle, &error) < 0) {
        dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
        return DPI_FAILURE;
    }

    // get the top level parameter descriptor
    if (dpiOci__attrGet(describeHandle, DPI_OCI_HTYPE_DESCRIBE, &topLevelParam,
            0, DPI_OCI_ATTR_PARAM, "get top level param", &error) < 0) {
        dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
        return DPI_FAILURE;
    }

    // get the attribute list parameter descriptor
    if (dpiOci__attrGet(topLevelParam, DPI_OCI_DTYPE_PARAM,
            (void*) &attrListParam, 0, DPI_OCI_ATTR_LIST_TYPE_ATTRS,
            "get attr list param", &error) < 0) {
        dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
        return DPI_FAILURE;
    }

    // create attribute structure for each attribute
    for (i = 0; i < objType->numAttributes; i++) {
        if (dpiOci__paramGet(attrListParam, DPI_OCI_DTYPE_PARAM, &attrParam,
                (uint32_t) i + 1, "get attribute param", &error) < 0) {
            dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
            return DPI_FAILURE;
        }
        if (dpiObjectAttr__allocate(objType, attrParam, &attributes[i],
                &error) < 0) {
            dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);
            return DPI_FAILURE;
        }
    }

    // free the describe handle
    dpiOci__handleFree(describeHandle, DPI_OCI_HTYPE_DESCRIBE);

    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType_getInfo() [PUBLIC]
//   Return information about the object type.
//-----------------------------------------------------------------------------
int dpiObjectType_getInfo(dpiObjectType *objType, dpiObjectTypeInfo *info)
{
    dpiError error;

    if (dpiGen__startPublicFn(objType, DPI_HTYPE_OBJECT_TYPE, __func__,
            &error) < 0)
        return DPI_FAILURE;
    DPI_CHECK_PTR_NOT_NULL(info)
    info->name = objType->name;
    info->nameLength = objType->nameLength;
    info->schema = objType->schema;
    info->schemaLength = objType->schemaLength;
    info->isCollection = objType->isCollection;
    info->elementTypeInfo = objType->elementTypeInfo;
    info->numAttributes = objType->numAttributes;
    return DPI_SUCCESS;
}


//-----------------------------------------------------------------------------
// dpiObjectType_release() [PUBLIC]
//   Release a reference to the object type.
//-----------------------------------------------------------------------------
int dpiObjectType_release(dpiObjectType *objType)
{
    return dpiGen__release(objType, DPI_HTYPE_OBJECT_TYPE, __func__);
}

