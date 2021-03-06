#
# Simple example: combines data from two existing datasets
#
# To embed it in an existing HDF5 file, run:
# $ make files
# $ hdf5-udf example-add_datasets.h5 example-add_datasets.py
#
# Note the absence of an output Dataset name in the call to
# hdf5-udf: the tool determines it based on the calls to
# lib.getData() made by this code. The resolution and
# data types are determined to be the same as that of the
# input datasets, Dataset1 and Dataset2.
#

def dynamic_dataset():
    ds1_data = lib.getData("Dataset1")
    ds2_data = lib.getData("Dataset2")
    udf_data = lib.getData("VirtualDataset")
    udf_dims = lib.getDims("VirtualDataset")

    for i in range(udf_dims[0] * udf_dims[1]):
        udf_data[i] = ds1_data[i] + ds2_data[i]
