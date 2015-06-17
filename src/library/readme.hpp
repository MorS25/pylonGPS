/**
\defgroup Server
\defgroup Serialization 
\defgroup Events
*/

/**
\mainpage
***************INTRODUCTION

Hello and welcome.

We are just starting to see the deployment of robots in a commercial setting and most of the pieces are in place for Robotics as a Service. This project is dedicated to making it even easier to do interesting and useful robotics applications using inexpensive differential GPS.

Differential GPS gives enhanced accuracy (<10 cm radius vs the normal 3000 cm radius) which can enable applications such as landing a quadcopter on a small platform or keeping a mobile robot on the sidewalk. It works by having a nearby GPS basestation with a known location that can tell the mobile unit how to compensate for the errors that the basestation has observed in the GPS signal.

Pylon GPS makes it easy to share GPS updates from your basestation and allows mobile units to get updates from any nearby basestations. This means that when a basestation with Pylon GPS is deployed, everyone within 10 km of the basestation can get updates and create applications using differential GPS. It works by maintaining a central server with a list of basestations and relaying laying updates using the NTRIP protocol. Future versions of the software will make it easier to search and host your own update sharing servers using Protobuf and ZeroMQ for serialization and network transport. 

The first draft of this code focused on providing an NTRIP 1.0 (http://www.wsrn3.org/CONTENT/Reference/Reference_NTRIP-V1-Tech-paper.pdf) compatible caster and server that can be used with RTKLIB.  During development of the first draft, it was discovered that the NTRIP 1.0 protocol doesn't support automatic registration of basestation sources (information about the sources are assumed to be entered manually by the owners of the NTRIP Caster server).  To enable easy sharing, a second interface was added to the PylonGPS caster which enable automatic addition of the basestation source "metadata" that was expected to be manually added.  As the software is currently implemented, this interface will be bound to a port number one higher than the one assigned to the PylonGPS NTRIP caster.  If the PylonGPS server software is used to forward basestation data, this registration will be carried out automatically.

***************Compilation

The next few minor releases will focus on making it easier to install the software by providing Debian packages and Windows installers.  For now, the preferred method is building from source.  So far, building and running has been tested on Ubuntu Linux and the instructions focus on that accordingly.

The PylonGPS source can be downloaded via: 
https://github.com/charlesrwest/pylonGPS/archive/master.zip

Alternatively, it can be cloned from GitHub using the following git command:
git clone https://github.com/charlesrwest/pylonGPS.git

Required libraries/utilities:
G++ with support for C++11 (most systems support this)
CMake (http://www.cmake.org/)
Doxygen (http://www.stack.nl/~dimitri/doxygen/ if documentation is desired)
Protobuf (https://developers.google.com/protocol-buffers/)
ZeroMQ (http://zeromq.org/)
POCO (http://pocoproject.org/)

Embedded Libraries:
ZeroMQ's header only C++ extension (https://github.com/zeromq/cppzmq)
Catch, a header only unit test framework (https://github.com/philsquared/Catch)

On Ubuntu Linux, it should be possible to install all required dependencies with the following command:

sudo apt-get install build-essential cmake doxygen protobuf-compiler libprotobuf-dev libzmq3-dev libpoco-dev

Compilation can be carried out via the following commands performed in the downloaded PylonGPS directory:
cmake ./
make
make doc

The "make doc" command is only nessesary if access to nice documentation is desired.  This step generates html files which allow browsing the code documentation in a web browser via the /pylonGPS/doc/html/index.html file (just double click on the file and it should open a browser).  The linkToGeneratedDocumentation.html file has been added to the top level directory to make it easier to get the the generated index file.

The integrity of the code can be checked via running the ./bin/unitTests executable.  If some of the unit tests fail on your platform, please email crwest@ncsu.edu with a screenshot of your error and a description of your operating enviroment.

***************Using PylonGPS

PylonGPS currently generates 3 executables, one shared library and one static library.  One of the executables is meant to unit test the code and will probably not be used by most people.  The static library is integrated into the shared library and isn't really used outside of the build process.

./lib/libpylongps.so:
This shared library includes most of the functionality in the PylonGPS software and is linked to by all of the executables.

./bin/ntripCaster:
This is a full ntripCaster that also supports automatic source registration via a ZMQ/Protobuf socket.  Once it is running, other NTRIP 1.0 compatible sources and clients can connect to it to broadcast and receive differential GPS updates.  It doesn't currently offer a way to manually add metadata, so sources need to use the ./bin/ntripServer program to register sources for now.
Usage:
./bin/ntripCaster portNumberToBind optionalMaximumNumberOfThreads optionalMaximumNumberOfConnectionsToQueue

portNumberToBind:
What port the caster program should bind and make the NTRIP caster available on.  The current implementation will also bind (portNumberToBind+1) for the metadata registration port.

optionalMaximumNumberOfThreads: This is the maximum number of threads that the server will run to accommodate client and server connections.  This could potentially be very large and is only really limited by OS limits on the per process number of threads and open file handles.  The default (if left empty) is 1000.

optionalMaximumNumberOfConnectionsToQueue: This is the maximum number of connections that can be waiting to be serviced before the server starts closing them immediately rather than queueing them.  The default (if left empty) is 1000.

./bin/ntripServer:
The purpose of this program is to allow a basestation to be able to automatically register a source of GPS corrections with a PylonGPS NTRIP caster.  It was designed with RTKLIB in mind.  The normal process of use would be to start an ntripServer and then forward the updates from RTKLIB to the ntripServer, which would then send it to the NTRIP caster to be advertised and broadcasted.  The server will connect and register with the caster as soon as something connects to its TCP port.
Usage:
./bin/ntripServer portNumberToBind casterURI ntripFormatSourceMetadata

portNumberToBind: The TCP port to bind (will probably want to be higher than 1024).  This is the port that should be given to RTKLIB as the target to send the data to.

casterURI: This is the contact information for the PylonGPS caster to send to.  It is similar to a URL and should take the form of ntrip://urlOrIPAddress:portNumber

ntripFormatSourceMetadata: This is the "metadata" about the source that will show up in the caster's NTRIP source table to represent the source.  It is given in the format below, which just so happens to be a very similar format to the one for NTRIP 1.0 source entries in the table.  Don't forget to use single quotes for this field or the terminal will try to run it as a command.

Example:

./bin/ntripServer 9001 ntrip://74.125.30.99:99 'STR;NCStateBasestation;CAND;ZERO;0;2;GPS;PBO;USA;35.939350;239.566310;0;0;TRIMBLE NETRS;none;B;N;5000;none' 

ntripFormatSourceMetadata Format:

Fields are delineated by ";" characters.  Some fields can be left blank, but the delimiters are required.

(1)   TYPE: 3 characters, always STR

(2)   MOUNTPOINT: ID of correction source, max 100 characters

(3)   IDENTIFIER: Some description, such as source city (arbitrary length)

(4)   FORMAT: What form the data is, such as 'RTCM 2.1' (arbitrary length)

(5)   FORMAT-DETAILS: Undefined extra details, such as update rate (arbitrary length)

(6)   CARRIER: Information about phase content.  '0' for no phase, '1' for L1, '2' for L1 and L2

(7)   NAVIGATION SYSTEMS: Descriptions about GPS, GPS+GLO, etc (arbitrary length)

(8)   NETWORK: Network this basestation is a part of (arbitrary length)

(9)   COUNTRY: ISO 3166 country code (3 characters)

(10) LATITUDE: Latitude of source

(11) LONGITUDE: Longitude of source

(12) NMEA: If client is required to send a NMEA string with their 
approximate position when trying to get information from this source (currently not implemented).  '0' if NMEA should not be sent and '1' if it should.

(13) SOLUTION: Stream is generated by a network or a single station. '0' if single basestation and '1' otherwise.

(14) GENERATOR: Indication of generated method (hardware or software stream (arbitrary length)

(15) COMPRESSION: Compression algorithm used, if any (arbitrary length)

(16) AUTHENTICATION: Indication if access protection is in use for this source (not currently implemented). 'N' for none, 'B' for password, 'D' for digest based.

(17) FEE: Indication if user will be charged for using the network (not currently implemented). 'N' if there is no charge and 'Y' if there is.

(18) BITRATE: Bits per second expected from the source (arbitrary length integer, not enforced)

(19) MISC: Any other information to include (arbitrary length)


***************Future Versions

RTKLIB supports streaming differential GPS updates to and from arbitrary TCP sockets.  This means that it would be possible to transport these updates without needing to use the HTTP based NTRIP protocol for registration/other functions if a PylonGPS program is the source/destination of these streams.  In the next major release, it is likely that PylonGPS will switch to a interface using ZMQ/Protobuf for communication and serialization while allowing access via both NTRIP and the open PylonGPS format at the caster (a model/view pattern).  This would allow new features, such as searching and proxying, to be added without having to redefine a standard every time (since Protobuf allows addition of new fields without breaking backwards compatibility).

*/ 
