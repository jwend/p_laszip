p_laszip

Background:

The LAStools laszip application was extended with MPI to allow the application
to be run in parallel on a compute cluster. The goal was an application that 
would scale to arbitrarily large input, limited only by the amount of 
disk space needed to store the input and output file. No intermediate files 
are generated and individual process memory needs are not determined by the 
size of the input or output.

The algorithm_and_results.pdf slide presentation contains a description 
of the implementation and test results of compressing and uncompressing a 
111GB las file on a compute cluster using 100 cores.

Dependencies:
An MPI implementation must be installed. OpenMPI 1.6 and 1.8 are known to work
and were used in development and testing. mpic++ must be found in your PATH. 

Install:

git clone https://github.com/jwend/p_laszip
cd p_laszip
make

The p_laszip executable is in the bin directory.

Test:

mpirun -n 3 bin/p_laszip -i data/test.las -o test.laz
mpirun -n 3 bin/p_laszip -i test.laz -o test.las
diff data/test.las test.las

Limitations and Supported Features:

p_laszip works only with LAS version 1.0, 1.1, and 1.2 and produces only 
corresponding LAZ file output. Version 1.2 was tested most extensively with 
up to the 111 GB file size input and output. 

Know Bugs:
p_laszip will fail in the total number of point chunks is less than 
the number of processes. That is, if chunk_count < process count. The current
default chunk_size is 50000 points. 












