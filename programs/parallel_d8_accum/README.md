Barnes2016-DistributedPriorityFlood
===================================

**Title of Manuscript**:
Flexibly Distributed Priority-Flood Depression Filling For Teracell DEMs

**Authors**: Richard Barnes

**Corresponding Author**: Richard Barnes (rbarnes@umn.edu)

**DOI Number of Manuscript**: [arXiv 1608.04431](https://arxiv.org/abs/1608.04431)

**Code Repositories**
 * [Author's GitHub Repository](https://github.com/r-barnes/Barnes2016-ParallelPriorityFlood)
 * [Journal's GitHub Repository](TODO)

This repository contains a reference implementation of the algorithms presented
in the manuscript above, along with information on acquiring the various
datasets used, and code to perform correctness tests.




Abstract
--------
High-resolution digital elevation models (DEMs) are increasingly available;
however, attempts to use these in existing hydroanalysis algorithms has created
problems which take too long to solve and which are too big to fit into a
computer's working memory. This has led to research on parallel algorithms and
algorithms which explicitly manage memory. Parallel approaches do not scale well
because they require many nodes and frequent communication between these nodes.
Memory-managing algorithms have to read and write subdivisions of the data
numerous times because they suffer from low access locality. Here, I adopt a
tile-based approach which unifies the parallel and memory-managing paradigms. I
show that it is possible to **perform depression-filling on a tiled DEM with a
fixed number of memory access and communication events per tile**, regardless of
the size of the DEM. The result is an algorithm that works equally well on one
core, multiple cores, or multiple machines and can take advantage of large
memories or cope with small ones. The largest dataset on which I run the
algorithm has 2 trillion (2*10^12) cells. With 48 cores, processing required 291
minutes to completion and 9.4 compute-days. This test is three orders of
magnitude larger than any previously performed in the literature, but took only
1-2 orders of magnitude more compute-time. Complete, well-commented source code
and correctness tests are available for download from a repository.





Compilation
-----------

The program can be produced simply by running **make**. However, certain
prerequisites are necessary for this to be successful.

For **compilation**, the following command will set you up on a Debian-based
system:

    sudo apt-get install make openmpi-bin libgdal-dev libopenmpi-dev

If you wish (as I did) to compile the code on XSEDE, certain modules must be
loaded:

    module load intel/2015.2.164
    module load mvapich2_ib

Note that temporary files can be stored in:

    /oasis/scratch/comet/$USER/temp_project

or some similar directory.

Running `make` will produce an executable called `parallel_pf.exe`.

Running the above compiles the program to run the _cache_ strategy. Using `make
compile_with_compression` will enable the _cacheC_ strategy instead. This
strategy is not compiled by default because it requires the Boost Iostreams
library. This libary can be installed with:

    sudo apt-get install libboost-iostreams-dev



Running the Program
-------------------

`parallel_pf.exe` can be run without arguments from the command line to show a
comprehensive explanation of the program and its options. This same text is in
the file `help.txt`.

In order to process data, you will need to run `parallel_pf.exe` in MPI. For
example:

    mpirun -n 4 ./parallel_pf.exe one @offloadall dem.tif outroot -w 500 -h 500

In the foregoing example `-n 4` indicates that the program should be run in
parallel over four processes. One of these processes (the one with MPI rank #0)
acts as a master process. It does limited computation but stores information
from all of the other processes. This requires less memory than one would think,
as discussed in the manuscript.



Layout Files
------------

A layout file is a text file with the format:

    file1.tif, file2.tif, file3.tif,
    file4.tif, file5.tif, file6.tif, file7.tif
             , file8.tif,          ,

where each of fileX.tif is a tile of the larger DEM collectively described by
all of the files. All of fileX.tif must have the same shape; the layout file
specifies how fileX.tif are arranged in relation to each other in space.
Blanks between commas indicate that there is no tile there: the algorithm will
treat such gaps as places to route flow towards (as if they are oceans). Note
that the files need not have TIF format: they can be of any type which GDAL
can read. Paths to fileX.tif are taken to be relative to the layout file.

Several example layout files are included in the `tests/` directory and end with
the `.layout` extension.



MPI Profiling
-------------
Although the program tracks its total communication load internally, I have also
used [mpiP](http://mpip.sourceforge.net/) to profile the code's communication.
The code can be downloaded [here](http://mpip.sourceforge.net/) and compiled
with:

    ./configure --with-binutils-dir=/usr/lib
    make shared
    make install #Installs to a subdirectory of mpiP

Prerequisites include: `binutils-dev`.

mpiP can be used to profile _any_ MPI program without the need to compile it
with the program. To do so, run the following line immediately before launching
`mpirun`:

    export LD_PRELOAD=path/to/libmpiP.so

Although the program tracks its maximum memory requirements internally, I have
also used `/usr/bin/time` to record this. An example of such an invocation is:

    mpirun -output-filename timing -n 4 /usr/bin/time -v ./parallel_pf.exe one @offloadall dem.tif outroot -w 500 -h 500

This will store memory and timing information in files beginning with the stem
`timing`.



Correctness
-----------

The correctness of this algorithm for small cases is established by comparing it
against the hand-made flow accumulation testing suite in `tests/flow_accum`.

The correctness for larger/real-world cases is established by comparing the
algorithm's output against a simpler flow accumulation. The tests below
accomplish this.


Testing
-------

For **running tests**, the following command will set you up on a Debian-based
system:

    sudo apt-get install python3-gdal python-gdal gdal-bin

Several tests are included.

###Test 1

Run

    make test
    ./test.exe

to check various indexing mathematics used in the program. If no warnings
appear, everything is good.

###Test 2

This test checks the simple flow accumulation DEMs in RichDEM's
`tests/flow_accum` suite. To run the test, do as follows:

    cd $RICHDEM_BASE_DIR/apps
    make #Compile all of the apps

    cd $PARALLEL_D8_ACCUM
    mkdir temp
    ./test_small.sh

where the directories are the obvious choices. If no warnings are printed, all
the tests pass. Additionally, running

    ./test_small.sh test 10

would run a test with `$RICHDEM_BASE_DIR/tests/flow_accum/testdem10.d8` as
input.

###Test 3

This test checks more complicated flow situations. You'll need to generate a
larger test DEM to run the test on. You can do so using the code found in
`tests/terrain_gen`. Assuming you've compiled the code there, run, e.g.:

    ../../tests/terrain_gen/terrain_gen.exe temp/testdem.tif 3000

You'll then need to generate flow directions from this DEM. You can do so using,
e.g.:

    ../../apps/rd_flood_for_flowdirs.exe temp/testdem.tif temp/flowdirs.tif

You can now run the big tests on this single file using, e.g.:

    ./test_big.py --one --cores 4 temp/flowdirs.tif


Notes
-----

The `communication.hpp` header file abstracts all of the MPI commands out of
`main.cpp`. This is useful for generating communication statistics, but also
preempts a day when the message passing is reimplemented using `std::threads` so
that the program can be compiled for use on a single node/desktop without having
to include MPI as a dependency.


RichDEM
-------

This code is part of the RichDEM codebase, which includes state of the art
algorithms for quickly performing hydrologic calculations on raster digital
elevation models. The full codebase is available at
[https://github.com/r-barnes](https://github.com/r-barnes)



TODO
----

Different MPI Polling methods
https://stackoverflow.com/questions/14560714/probe-seems-to-consume-the-cpu