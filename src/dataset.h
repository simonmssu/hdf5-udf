/*
 * HDF5-UDF: User-Defined Functions for HDF5
 *
 * File: dataset.h
 *
 * High-level interfaces for information retrieval from HDF5 datasets.
 */
#ifndef __dataset_h
#define __dataset_h

#include <hdf5.h>
#include <vector>
#include <string>
#include <numeric>
#include <sstream>

struct DatasetTypeInfo {
    DatasetTypeInfo(std::string dtype, std::string ddeclaration, hid_t did, hid_t dsize) :
        datatype(dtype),
        declaration(ddeclaration),
        hdf5_datatype_id(did),
        datatype_size(dsize) { }

    std::string datatype;
    std::string declaration;
    hid_t hdf5_datatype_id;
    hid_t datatype_size;
};

/* Dataset information */
class DatasetInfo {
public:
    DatasetInfo();
    DatasetInfo(std::string in_name, std::vector<hsize_t> in_dims, std::string in_datatype);

    size_t getGridSize() const;
    const char *getDatatype() const;
    size_t getHdf5Datatype() const;
    hid_t getStorageSize() const;
    const char *getCastDatatype() const;
    void printInfo(std::string dataset_type) const;

    std::string name;                /* Dataset name */
    std::string datatype;            /* Datatype, given as string */
    std::string dimensions_str;      /* Dimensions, given as string */
    hid_t hdf5_datatype;             /* Datatype, given as HDF5 type */
    std::vector<hsize_t> dimensions; /* Dataset dimensions */
    void *data;                      /* Allocated buffer to hold dataset data */
};

#endif /* __dataset_h */
