#include "caster.hpp"

using namespace pylongps;

/**
This function initializes the class, creates the associated database, and starts the two threads associated with it.
@param inputContext: The ZMQ context that this object should use
@param inputCasterID: The 64 bit ID associated with this caster (make sure it does not collide with caster IDs the clients are likely to run into)
@param inputTransmitterRegistrationAndStreamingPortNumber: The port number to register the ZMQ router socket used for receiving PylonGPS transmitter registrations/streams
@param inputAuthenticatedTransmitterRegistrationAndStreamingPortNumber: The port number to register the ZMQ router socket used for receiving authenticated PylonGPS transmitter registrations/streams
@param inputClientRequestPortNumber: The port to open to receive client requests (including requests from proxies for the list of sources)
@param inputClientStreamPublishingPortNumber: The port to open for the interface to publish stream data to clients
@param inputProxyStreamPublishingPortNumber: The port to open for the interface to publish stream data to proxies (potentially higher priority)
@param inputStreamStatusNotificationPortNumber: The port for the interface that is used to to publish stream status changes
@param inputCasterPublicKey: The public key for this caster to use for encryption/authentation
@param inputCasterSecretKey: The secret key for this caster to use for encryption/authentation
@param inputCasterSQLITEConnectionString: The connection string used to connect to or create the SQLITE database used for stream source entry management and query resolution.  If an empty string is given (by default), it will connect/create an in memory database with a random 64 bit hex number string (example: "file:jfuggekdai?mode=memory&cache=shared")

@throws: This function can throw exceptions
*/
caster::caster(zmq::context_t *inputContext, int64_t inputCasterID, uint32_t inputTransmitterRegistrationAndStreamingPortNumber, uint32_t inputAuthenticatedTransmitterRegistrationAndStreamingPortNumber, uint32_t inputClientRequestPortNumber, uint32_t inputClientStreamPublishingPortNumber, uint32_t inputProxyStreamPublishingPortNumber, uint32_t inputStreamStatusNotificationPortNumber, const std::string &inputCasterPublicKey, const std::string &inputCasterSecretKey, const std::string &inputCasterSQLITEConnectionString) : databaseConnection(nullptr, &sqlite3_close_v2)
{
if(inputContext == nullptr)
{
throw SOMException("Invalid ZMQ context\n", INVALID_FUNCTION_INPUT, __FILE__, __LINE__);
}

//Check that keys are the right size
if((inputCasterPublicKey.size() != 32 && inputCasterPublicKey.size() != 40) || (inputCasterSecretKey.size() != 32 && inputCasterSecretKey.size() != 40))
{
throw SOMException("Invalid ZMQ key(s)\n", INVALID_FUNCTION_INPUT, __FILE__, __LINE__);
}

//Save given parameters
context = inputContext;
casterID = inputCasterID;
transmitterRegistrationAndStreamingPortNumber = inputTransmitterRegistrationAndStreamingPortNumber;
authenticatedTransmitterRegistrationAndStreamingPortNumber = inputAuthenticatedTransmitterRegistrationAndStreamingPortNumber;
clientRequestPortNumber = inputClientRequestPortNumber;
clientStreamPublishingPortNumber = inputClientStreamPublishingPortNumber;
proxyStreamPublishingPortNumber = inputProxyStreamPublishingPortNumber;
streamStatusNotificationPortNumber = inputStreamStatusNotificationPortNumber;
casterPublicKey = inputCasterPublicKey;
casterSecretKey = inputCasterSecretKey;

//Set database connection string 
if(inputCasterSQLITEConnectionString == "")
{
//Generate random 64 bit unsigned int
std::random_device randomnessSource;
std::uniform_int_distribution<uint64_t> distribution;
uint64_t connectionInteger = distribution(randomnessSource);

databaseConnectionString = "file:" + std::to_string(connectionInteger) + "?mode=memory&cache=shared";
}
else
{
databaseConnectionString = inputCasterSQLITEConnectionString;
}

//Attempt to connect to the database
sqlite3 *databaseConnectionBuffer = nullptr;
if(sqlite3_open_v2(databaseConnectionString.c_str(), &databaseConnectionBuffer, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,NULL) != SQLITE_OK)
{
throw SOMException("Unable to open database connection\n", SQLITE3_ERROR, __FILE__, __LINE__);
}
databaseConnection.reset(databaseConnectionBuffer); //Transfer ownership so that connection will be closed when object goes out of scope

//Make sure foreign key relationships will be honored
if(sqlite3_exec(databaseConnection.get(), "PRAGMA foreign_keys = on;", NULL, NULL, NULL) != SQLITE_OK)
{
throw SOMException("Error enabling foreign keys\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

//Load custom SQLite functions (sin, cos, acos) for great circle calculations
if(sqlite3_create_function(databaseConnection.get(), "sin",1, SQLITE_UTF8, NULL, &pylongps::SQLiteSinFunctionDegrees, NULL, NULL ) != SQLITE_OK)
{
throw SOMException("Error adding SQLite3 function\n", SQLITE3_ERROR, __FILE__, __LINE__);
}
if(sqlite3_create_function(databaseConnection.get(), "cos",1, SQLITE_UTF8, NULL, &pylongps::SQLiteCosFunctionDegrees, NULL, NULL ) != SQLITE_OK)
{
throw SOMException("Error adding SQLite3 function\n", SQLITE3_ERROR, __FILE__, __LINE__);
}
if(sqlite3_create_function(databaseConnection.get(), "acos",1, SQLITE_UTF8, NULL, &pylongps::SQLiteAcosFunctionDegrees, NULL, NULL ) != SQLITE_OK)
{
throw SOMException("Error adding SQLite3 function\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

//Setup converter so that queries can be processed
SOM_TRY
setupBaseStationToSQLInterface();
SOM_CATCH("Error setting up basestationToSQLInterface\n")


//Initialize and bind shutdown socket
SOM_TRY
shutdownPublishingSocket.reset(new zmq::socket_t(*(context), ZMQ_PUB));
SOM_CATCH("Error intializing shutdownPublishingSocket\n")

int extensionStringNumber = 0;
SOM_TRY //Bind to an dynamically generated address
std::tie(shutdownPublishingConnectionString,extensionStringNumber) = bindZMQSocketWithAutomaticAddressGeneration(*shutdownPublishingSocket, "shutdownPublishingSocketAddress");
SOM_CATCH("Error binding shutdownPublishingSocket\n")

//Initialize, connect and subscribe sockets which listen for shutdown signal
std::vector<std::unique_ptr<zmq::socket_t> *> signalListeningSockets = {&clientRequestHandlingThreadShutdownListeningSocket, &streamRegistrationAndPublishingThreadShutdownListeningSocket, &authenticationIDCheckingThreadShutdownListeningSocket, &statisticsGatheringThreadShutdownListeningSocket};

for(int i=0; i<signalListeningSockets.size(); i++)
{
SOM_TRY //Make socket
(*signalListeningSockets[i]).reset(new zmq::socket_t(*(context), ZMQ_SUB));
SOM_CATCH("Error creating socket to listen for shutdown signal\n")

SOM_TRY //Connect to hear signal
(*signalListeningSockets[i])->connect(shutdownPublishingConnectionString.c_str());
SOM_CATCH("Error connecting socket to listen for shutdown signal\n")

SOM_TRY //Set filter to allow any published messages to be received
(*signalListeningSockets[i])->setsockopt(ZMQ_SUBSCRIBE, nullptr, 0);
SOM_CATCH("Error setting subscription for socket listening for shutdown signal\n")
}

//Initialize and bind database access socket
SOM_TRY
databaseAccessSocket.reset(new zmq::socket_t(*(context), ZMQ_PUB));
SOM_CATCH("Error intializing shutdownPublishingSocket\n")

SOM_TRY //Bind to an dynamically generated address
std::tie(databaseAccessConnectionString,extensionStringNumber) = bindZMQSocketWithAutomaticAddressGeneration(*databaseAccessSocket, "databaseAccessAddress");
SOM_CATCH("Error binding databaseAccessSocket\n")

//Attempt to register socket for authentication ID verification via ZAP protocol (could already be registered by another object)
SOM_TRY
ZAPAuthenticationSocket.reset(new zmq::socket_t(*(context), ZMQ_REP));
SOM_CATCH("Error intializing ZAPAuthenticationSocket\n")

SOM_TRY
try
{
ZAPAuthenticationSocket->bind("inproc://zeromq.zap.01");
}
catch(const zmq::error_t &inputError)
{
if(inputError.num() == EADDRINUSE)
{
ZAPAuthenticationSocket.reset(nullptr); //There is already a ZAP handler, so we don't need one 
}
else
{
throw; //It was some other error, so pass the exception along
}
}
SOM_CATCH("Error binding ZAP inproc socket\n")

//Initialize and bind transmitterRegistrationAndStreaming socket
SOM_TRY
transmitterRegistrationAndStreamingInterface.reset(new zmq::socket_t(*(context), ZMQ_ROUTER));
SOM_CATCH("Error intializing transmitterRegistrationAndStreamingInterface\n")

SOM_TRY
std::string bindingAddress = "tcp://*:" + std::to_string(transmitterRegistrationAndStreamingPortNumber);
transmitterRegistrationAndStreamingInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding transmitterRegistrationAndStreamingInterface\n")

//Initialize and bind authenticatedTransmitterRegistrationAndStreamingPortNumber socket
SOM_TRY
authenticatedTransmitterRegistrationAndStreamingInterface.reset(new zmq::socket_t(*(context), ZMQ_ROUTER));
SOM_CATCH("Error intializing authenticatedTransmitterRegistrationAndStreamingInterface\n")

SOM_TRY //Set socket mode to CURVE server for authentication/encryption
int serverRole = 1;
authenticatedTransmitterRegistrationAndStreamingInterface->setsockopt(ZMQ_CURVE_SERVER, &serverRole, sizeof(serverRole));
SOM_CATCH("Error setting authenticatedTransmitterRegistrationAndStreamingInterface role\n")

SOM_TRY //Set secret key for socket
authenticatedTransmitterRegistrationAndStreamingInterface->setsockopt(ZMQ_CURVE_SECRETKEY, casterSecretKey.c_str(), casterSecretKey.size());
SOM_CATCH("Error setting secret key for caster\n")

SOM_TRY //Bind port for socket
std::string bindingAddress = "tcp://*:" + std::to_string(authenticatedTransmitterRegistrationAndStreamingPortNumber);
transmitterRegistrationAndStreamingInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding transmitterRegistrationAndStreamingInterface\n")

//Initialize and bind clientRequestInterface socket
SOM_TRY
clientRequestInterface.reset(new zmq::socket_t(*(context), ZMQ_ROUTER));
SOM_CATCH("Error intializing clientRequestInterface\n")

SOM_TRY
std::string bindingAddress = "tcp://*:" + std::to_string(clientRequestPortNumber);
clientRequestInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding clientRequestInterface\n")

//Initialize and bind clientStreamPublishingInterface socket
SOM_TRY
clientStreamPublishingInterface.reset(new zmq::socket_t(*(context), ZMQ_PUB));
SOM_CATCH("Error intializing clientStreamPublishingInterface\n")

SOM_TRY
std::string bindingAddress = "tcp://*:" + std::to_string(clientStreamPublishingPortNumber);
clientStreamPublishingInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding clientStreamPublishingInterface\n")

//Initialize and bind proxyStreamPublishingInterface socket
SOM_TRY
proxyStreamPublishingInterface.reset(new zmq::socket_t(*(context), ZMQ_PUB));
SOM_CATCH("Error intializing proxyStreamPublishingInterface\n")

SOM_TRY
std::string bindingAddress = "tcp://*:" + std::to_string(proxyStreamPublishingPortNumber);
proxyStreamPublishingInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding proxyStreamPublishingInterface\n")

//Initialize and bind streamStatusNotificationInterface socket
SOM_TRY
streamStatusNotificationInterface.reset(new zmq::socket_t(*(context), ZMQ_PUB));
SOM_CATCH("Error intializing streamStatusNotificationInterface\n")

SOM_TRY
std::string bindingAddress = "tcp://*:" + std::to_string(streamStatusNotificationPortNumber);
streamStatusNotificationInterface->bind(bindingAddress.c_str());
SOM_CATCH("Error binding streamStatusNotificationInterface\n")

//Start threads
SOM_TRY
clientRequestHandlingThread.reset(new std::thread(&caster::clientRequestHandlingThreadFunction, this)); 
SOM_CATCH("Error initializing thread\n")

SOM_TRY
streamRegistrationAndPublishingThread.reset(new std::thread(&caster::streamRegistrationAndPublishingThreadFunction, this)); 
SOM_CATCH("Error initializing thread\n")

if(ZAPAuthenticationSocket.get() != nullptr)
{
SOM_TRY
authenticationIDCheckingThread.reset(new std::thread(&caster::authenticationIDCheckingThreadFunction, this)); 
SOM_CATCH("Error initializing thread\n")
}

SOM_TRY
statisticsGatheringThread.reset(new std::thread(&caster::statisticsGatheringThreadFunction, this)); 
SOM_CATCH("Error initializing thread\n")

}

/**
This function signals for the threads to shut down and then waits for them to do so.
*/
caster::~caster()
{
//Ensure that all exits points still unregister connector
//SOMScopeGuard connectorGuard([](){ Poco::Data::SQLite::Connector::unregisterConnector();});

//Publish shutdown signal and wait for threads
try
{ //Send empty message to signal shutdown
SOM_TRY
shutdownPublishingSocket->send(nullptr, 0);
SOM_CATCH("Error sending shutdown signal for caster threads\n")
}
catch(const std::exception &inputException)
{
fprintf(stderr, "%s", inputException.what());
}

//Wait for threads to finish
if(authenticationIDCheckingThread.get() != nullptr)
{
authenticationIDCheckingThread->join(); 
}

clientRequestHandlingThread->join();
streamRegistrationAndPublishingThread->join();
statisticsGatheringThread->join();
}

/**
This function is called in the clientRequestHandlingThread to handle client requests and manage access to the SQLite database.
*/
void caster::clientRequestHandlingThreadFunction()
{
try
{
//Responsible for databaseAccessSocket, clientRequestInterface and clientRequestHandlingThreadShutdownListeningSocket

//Create priority queue for event queue and poll items, then start polling/event cycle
std::priority_queue<event> threadEventQueue;
std::unique_ptr<zmq::pollitem_t[]> pollItems;
int numberOfPollItems = 3;

SOM_TRY
pollItems.reset(new zmq::pollitem_t[numberOfPollItems]);
SOM_CATCH("Error creating poll items\n")

//Populate the poll object list
pollItems[0] = {(void *) (*databaseAccessSocket), 0, ZMQ_POLLIN, 0};
pollItems[1] = {(void *) (*clientRequestInterface), 0, ZMQ_POLLIN, 0};
pollItems[2] = {(void *) (*clientRequestHandlingThreadShutdownListeningSocket), 0, ZMQ_POLLIN, 0};


//Determine if an event has timed out (and deal with it if so) and then calculate the time until the next event timeout
Poco::Timestamp nextEventTime;
int64_t timeUntilNextEventInMilliseconds = 0;
while(true)
{
nextEventTime = handleEvents(threadEventQueue);

if(nextEventTime < 0)
{
timeUntilNextEventInMilliseconds = -1; //No events, so block until a message is received
}
else
{
timeUntilNextEventInMilliseconds = (nextEventTime - Poco::Timestamp())/1000 + 1; //Time in milliseconds till the next event, rounding up
}

//Poll until the next event timeout and resolve any messages that are received
SOM_TRY
if(zmq::poll(pollItems.get(), numberOfPollItems, timeUntilNextEventInMilliseconds) == 0)
{
continue; //Poll returned without indicating any messages have been received, so check events and go back to polling
}
SOM_CATCH("Error polling\n")

//Handle received messages

//Check if it is time to shutdown
if(pollItems[2].revents & ZMQ_POLLIN)
{
return; //Shutdown message received, so return
}

//Check if a database request has been received
if(pollItems[0].revents & ZMQ_POLLIN)
{//A database request has been received, so process it
SOM_TRY
processDatabaseRequest();
SOM_CATCH("Error processing database request\n")
}

//Check if a client request has been received
if(pollItems[1].revents & ZMQ_POLLIN)
{//A client request has been received, so process it
SOM_TRY
processClientQueryRequest();
SOM_CATCH("Error processing request\n")
}

}

}
catch(const std::exception &inputException)
{ //If an exception is thrown, swallow it, send error message and terminate
fprintf(stderr, "clientRequestHandlingThreadFunction: %s\n", inputException.what());
return;
}

}

/**
This function is called in the streamRegistrationAndPublishingThread to handle stream registration and publishing updates.
*/
void caster::streamRegistrationAndPublishingThreadFunction()
{

}

/**
This function is called in the authenticationIDCheckingThread to verify if the ZMQ connection ID of authenticated connections matches the public key the connection is using.
*/
void caster::authenticationIDCheckingThreadFunction()
{ //TODO: finish this function

try
{
//Responsible for ZAPAuthenticationSocket, authenticationIDCheckingThreadShutdownListeningSocket

//Create priority queue for event queue and poll items, then start polling/event cycle
std::priority_queue<event> threadEventQueue;
std::unique_ptr<zmq::pollitem_t[]> pollItems;
int numberOfPollItems = 2;

SOM_TRY
pollItems.reset(new zmq::pollitem_t[numberOfPollItems]);
SOM_CATCH("Error creating poll items\n")

//Populate the poll object list
pollItems[0] = {(void *) (*ZAPAuthenticationSocket), 0, ZMQ_POLLIN, 0};
pollItems[1] = {(void *) (*authenticationIDCheckingThreadShutdownListeningSocket), 0, ZMQ_POLLIN, 0};

//Determine if an event has timed out (and deal with it if so) and then calculate the time until the next event timeout
Poco::Timestamp nextEventTime;
int64_t timeUntilNextEventInMilliseconds = 0;
while(true)
{
nextEventTime = handleEvents(threadEventQueue);

if(nextEventTime < 0)
{
timeUntilNextEventInMilliseconds = -1; //No events, so block until a message is received
}
else
{
timeUntilNextEventInMilliseconds = (nextEventTime - Poco::Timestamp())/1000 + 1; //Time in milliseconds till the next event, rounding up
}

//Poll until the next event timeout and resolve any messages that are received
SOM_TRY
if(zmq::poll(pollItems.get(), numberOfPollItems, timeUntilNextEventInMilliseconds) == 0)
{
continue; //Poll returned without indicating any messages have been received, so check events and go back to polling
}
SOM_CATCH("Error polling\n")

//Handle received messages

//Check if it is time to shutdown
if(pollItems[1].revents & ZMQ_POLLIN)
{
return; //Shutdown message received, so return
}

//Check if a database request has been received
if(pollItems[0].revents & ZMQ_POLLIN)
{//A ZAP authentication message (http://rfc.zeromq.org/spec:27) has been received, so process it (ZMQ identity must contain the 32 byte public key in the first 32 bytes of the identity, so that the key credentials can be checked at the router socket). 
SOM_TRY
processZAPAuthenticationRequest();
SOM_CATCH("Error processing database request\n")
}

}

}
catch(const std::exception &inputException)
{ //If an exception is thrown, swallow it, send error message and terminate
fprintf(stderr, "clientRequestHandlingThreadFunction: %s\n", inputException.what());
return;
}


}

/**
This function monitors published updates, collects the associated statistics and periodically reports them to the database.
*/
void caster::statisticsGatheringThreadFunction()
{

}

/**
This function processes any events that are scheduled to have occurred by now and returns when the next event is scheduled to occur.  Which thread is calling this function is determined by the type of events in the event queue.
@param inputEventQueue: The event queue to process events from
@return: The time point associated with the soonest event timeout (negative if there are no outstanding events)

@throws: This function can throw exceptions
*/
Poco::Timestamp caster::handleEvents(std::priority_queue<pylongps::event> &inputEventQueue)
{

while(true)
{//Process an event if its time is less than the current timestamp

if(inputEventQueue.size() == 0)
{//No events left, so return negative
return Poco::Timestamp(-1);
}

if(inputEventQueue.top().time > Poco::Timestamp() )
{ //Next event isn't happening yet
return inputEventQueue.top().time;
}

//There is an event to process 
//TODO: Process events 

}

}

/**
This function sets up the basestationToSQLInterface and generates the associated tables so that basestations can be stored and returned.  databaseConnection must be setup before this function is called.

@throws: This function can throw exceptions
*/
void caster::setupBaseStationToSQLInterface()
{
//Initialize object with database connection and table name
SOM_TRY
basestationToSQLInterface.reset(new protobufSQLConverter<base_station_stream_information>(databaseConnection.get(), "base_station_stream_information"));
SOM_CATCH("Error initializing basestationToSQLInterface\n")

//Register each of the fields with their associated database entries
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_DOUBLE_FIELD(base_station_stream_information, latitude, "latitude" ));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_DOUBLE_FIELD(base_station_stream_information, longitude, "longitude" ));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_DOUBLE_FIELD(base_station_stream_information, expected_update_rate, "expected_update_rate" ));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_ENUM_FIELD(base_station_stream_information, corrections_message_format, message_format, "message_format"));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_STRING_FIELD(base_station_stream_information, informal_name, "informal_name"));

basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_ENUM_FIELD(base_station_stream_information, base_station_class, station_class, "station_class"));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_INT64_FIELD(base_station_stream_information, base_station_id, "base_station_id"), true); //This field must be set in all all messages stored
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_STRING_FIELD(base_station_stream_information, source_public_key, "source_public_key"));
basestationToSQLInterface->addField(PYLON_GPS_GEN_REPEATED_STRING_FIELD(base_station_stream_information, signing_keys,  "signing_keys", "signing_key", "foreign_key"));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_DOUBLE_FIELD(base_station_stream_information, real_update_rate, "real_update_rate" ));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_DOUBLE_FIELD(base_station_stream_information, uptime, "uptime" ));
basestationToSQLInterface->addField(PYLON_GPS_GEN_OPTIONAL_INT64_FIELD(base_station_stream_information, start_time, "start_time"));

//Generate associated tables in database
SOM_TRY
basestationToSQLInterface->createTables();
SOM_CATCH("Error creating tables for basestationToSQLInterface")
}

/**
This function checks if the databaseAccessSocket has received a database_request message and (if so) processes the message and sends a database_reply in response.
@throws: This function can throw exceptions
*/
void caster::processDatabaseRequest()
{
//Receive message
std::unique_ptr<zmq::message_t> messageBuffer;

SOM_TRY
messageBuffer.reset(new zmq::message_t);
SOM_CATCH("Error initializing ZMQ message")

SOM_TRY //Receive message
if(databaseAccessSocket->recv(messageBuffer.get(), ZMQ_DONTWAIT) != true)
{
return; //No message to be had
}
SOM_CATCH("Error receiving server registration/deregistration message")

//Create lambda to make it easy to send request failed replies
auto sendReplyLambda = [&] (bool inputRequestFailed, enum database_request_failure_reason inputReason = DATABASE_REQUEST_DESERIALIZATION_FAILED)
{
database_reply reply;
std::string serializedReply;

if(inputRequestFailed)
{ //Only set the reason if the request failed
reply.set_reason(inputReason);
}
reply.SerializeToString(&serializedReply);

SOM_TRY
databaseAccessSocket->send(serializedReply.c_str(), serializedReply.size());
SOM_CATCH("Error sending reply message\n")
};

//Attempt to deserialize
database_request request;
request.ParseFromArray(messageBuffer->data(), messageBuffer->size());

if(!request.IsInitialized())
{
//Message header serialization failed, so send back message saying request failed
SOM_TRY
sendReplyLambda(true, DATABASE_REQUEST_DESERIALIZATION_FAILED);
return;
SOM_CATCH("Error sending reply");
}

//Validate submessage
if(request.has_base_station_to_register())
{//Add basestation
if(!request.base_station_to_register().has_latitude() || !request.base_station_to_register().has_longitude() || !request.base_station_to_register().has_base_station_id() || !request.base_station_to_register().has_start_time() || !request.base_station_to_register().has_message_format())
{//Message did not have required fields, so send back message saying request failed
SOM_TRY
sendReplyLambda(true, DATABASE_REQUEST_FORMAT_INVALID);
return;
SOM_CATCH("Error sending reply")
}

//Perform database operation
SOM_TRY //Attempt to store basestation in database
basestationToSQLInterface->store((*request.mutable_base_station_to_register()));
SOM_CATCH("Error inserting basestation to database\n")
//Send reply
SOM_TRY
sendReplyLambda(false); //Request succedded
SOM_CATCH("Error sending reply\n")
return;
}

if(request.has_delete_base_station_id() )
{//Perform delete operation
SOM_TRY
std::vector<::google::protobuf::int64> keysToDelete;
keysToDelete.push_back(request.delete_base_station_id());
basestationToSQLInterface->deleteObjects(keysToDelete);
SOM_CATCH("Error deleting from database\n")

SOM_TRY
sendReplyLambda(false); //Request succedded
SOM_CATCH("Error sending reply\n")
return;
}

if(request.has_base_station_to_update_id() && request.has_real_update_rate())
{ //Perform update operation
SOM_TRY
basestationToSQLInterface->update(fieldValue((::google::protobuf::int64) request.base_station_to_update_id()), "real_update_rate", fieldValue(request.real_update_rate()));
SOM_CATCH("Error updating database\n")

SOM_TRY
sendReplyLambda(false); //Request succedded
SOM_CATCH("Error sending reply\n")
return;
}

SOM_TRY
sendReplyLambda(true, DATABASE_REQUEST_FORMAT_INVALID); //Request failed
SOM_CATCH("Error sending reply\n")
}

/**
This function checks if the clientRequestInterface has received a client_query_request message and (if so) processes the message and sends a client_query_reply in response.
@throws: This function can throw exceptions
*/
void caster::processClientQueryRequest()
{
//Receive message
std::unique_ptr<zmq::message_t> messageBuffer;

SOM_TRY
messageBuffer.reset(new zmq::message_t);
SOM_CATCH("Error initializing ZMQ message")

SOM_TRY //Receive message
if(clientRequestInterface->recv(messageBuffer.get(), ZMQ_DONTWAIT) != true)
{
return; //No message to be had
}
SOM_CATCH("Error receiving server registration/deregistration message")

//Create lambda to make it easy to send request failed replies
auto sendReplyLambda = [&] (bool inputRequestFailed, enum client_query_request_failure_reason inputReason = CLIENT_QUERY_REQUEST_DESERIALIZATION_FAILED, ::google::protobuf::int64 inputCasterID = 0,  std::vector<base_station_stream_information> inputBaseStations = std::vector<base_station_stream_information>(0))
{
client_query_reply reply;
std::string serializedReply;

if(inputRequestFailed)
{ //Only set the reason if the request failed
reply.set_failure_reason(inputReason);
}
else
{
reply.set_caster_id(inputCasterID);
for(int i=0; i<inputBaseStations.size(); i++)
{
auto newObjectPointer = reply.mutable_base_stations()->Add();
newObjectPointer = &inputBaseStations[i];
}
}
reply.SerializeToString(&serializedReply);

SOM_TRY
clientRequestInterface->send(serializedReply.c_str(), serializedReply.size());
SOM_CATCH("Error sending reply message\n")
};

//Attempt to deserialize
client_query_request request;
request.ParseFromArray(messageBuffer->data(), messageBuffer->size());

if(!request.IsInitialized())
{
//Message header serialization failed, so send back message saying request failed
SOM_TRY
sendReplyLambda(true, CLIENT_QUERY_REQUEST_DESERIALIZATION_FAILED);
return;
SOM_CATCH("Error sending reply");
}

//TODO: establish limits on query complexity

int boundParameterCount = 0;
std::string sqlQueryString;
SOM_TRY
sqlQueryString = generateClientQueryRequestSQLString(request, boundParameterCount);
SOM_CATCH("Error generating request sql string\n")

if(boundParameterCount > 999)
{ //Too many bound parameters to process
SOM_TRY
sendReplyLambda(true, CLIENT_QUERY_REQUEST_TOO_COMPLEX);
return;
SOM_CATCH("Error sending reply");
}

std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> clientQueryStatement(nullptr, &sqlite3_finalize);

SOM_TRY
basestationToSQLInterface->prepareStatement(clientQueryStatement, sqlQueryString);
SOM_CATCH("Error preparing query statement\n")

int bindingParameterCount = bindClientQueryRequestFields(clientQueryStatement, request);
if(bindingParameterCount != boundParameterCount)
{
throw SOMException("Invalid ZMQ context\n", AN_ASSUMPTION_WAS_VIOLATED_ERROR, __FILE__, __LINE__);
}

int returnValue = sqlite3_step(clientQueryStatement.get());
sqlite3_reset(clientQueryStatement.get()); //Reset so that statement can be used again
if(returnValue != SQLITE_DONE)
{
throw SOMException("Error executing statement\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

//Check column number and type
if(sqlite3_column_count(clientQueryStatement.get()) !=1)
{
throw SOMException("Error, wrong number of result columns returned\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

if(sqlite3_column_type(clientQueryStatement.get(), 0) != SQLITE_INTEGER)
{
throw SOMException("Error, wrong type of result columns returned\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

int stepReturnValue = 0;
std::vector<::google::protobuf::int64> resultPrimaryKeys;
while(true)
{
int stepReturnValue = sqlite3_step(clientQueryStatement.get());

if(stepReturnValue == SQLITE_DONE)
{ //All results for this field have been retrieved
break; 
}

if(stepReturnValue != SQLITE_ROW)
{
throw SOMException("Error executing query\n", SQLITE3_ERROR, __FILE__, __LINE__);
}

resultPrimaryKeys.push_back((::google::protobuf::int64) sqlite3_column_int(clientQueryStatement.get(), 0));
}

std::vector<base_station_stream_information> results;
SOM_TRY
results = basestationToSQLInterface->retrieve(resultPrimaryKeys);
SOM_CATCH("Error retrieving objects associated with query primary keys\n")

Poco::Timestamp currentTime;
auto timeValue = currentTime.epochMicroseconds();
for(int i=0; i<results.size(); i++)
{
results[i].set_uptime(timeValue-results[i].start_time());
}

SOM_TRY //Send back query results
sendReplyLambda(false, CLIENT_QUERY_REQUEST_DESERIALIZATION_FAILED, casterID, results);
SOM_CATCH("Error sending reply\n")
}

/**
This process checks if ZAPAuthenticationSocket has received a ZAP request.  If so, it checks to make sure that the first 32 bytes of the connection identity contains the public key in the credentials, so that the key associated with a connection can be checked using the identity at the authenticatedTransmitterRegistrationAndStreamingInterface using a credentials message sent over that connection

@throws: This function can throw exceptions
*/
void caster::processZAPAuthenticationRequest()
{
//Create lambda to make it easy to send replies
auto sendReplyLambda = [&] (const std::string &inputRequestID, int inputStatusCode)
{ //Reply
std::vector<std::string> contentToSend;
contentToSend.push_back(""); //0. Zero length frame
contentToSend.push_back("1.0"); //1. "1.0"
contentToSend.push_back(inputRequestID); //2. requestID (echoed)
contentToSend.push_back(std::to_string(inputStatusCode)); //3. status code -> "200" for success, "300" for temp error, "400" for auth failure, "500" for internal error
contentToSend.push_back(""); //4. Empty or custom error message
contentToSend.push_back(""); //5. Could contain metadata about user if status is "200", must be empty otherwise
contentToSend.push_back(""); //6. Empty or ZMQ format defined style metadata message 

for(int i=0; i<contentToSend.size(); i++)
{
SOM_TRY
ZAPAuthenticationSocket->send(contentToSend[i].c_str(), contentToSend[i].size());
SOM_CATCH("Error sending reply message\n")
}

};

std::vector<std::string> receivedContent; //Save all of the multi part message in a vector
auto sendSystemFailedReply = [&] () 
{
if(receivedContent.size() >= 3)
{
SOM_TRY
sendReplyLambda(receivedContent[2],500);
SOM_CATCH("Error sending reply\n")
}
else
{
SOM_TRY
sendReplyLambda("",500);
SOM_CATCH("Error sending reply\n")
}
};

std::unique_ptr<zmq::message_t> messageBuffer;

while(true)
{
SOM_TRY //Receive message
messageBuffer.reset(new zmq::message_t);
SOM_CATCH("Error initializing ZMQ message")

bool messageReceived = false;
SOM_TRY //Receive message
messageReceived = ZAPAuthenticationSocket->recv(messageBuffer.get(), ZMQ_DONTWAIT);
SOM_CATCH("Error receiving server registration/deregistration message")

if(!messageReceived)
{
SOM_TRY
sendSystemFailedReply();
SOM_CATCH("Error sending reply\n")
}

receivedContent.push_back(std::string((const char *) messageBuffer->data(), messageBuffer->size()));

if(!messageBuffer->more())
{ //Multi-part message completed, so exit loop
break;
}

}

if(receivedContent.size() != 8)
{ //Received wrong number of messages
SOM_TRY
sendSystemFailedReply();
SOM_CATCH("Error sending reply\n")
}

//REP socket, should receive a 8 part message (http://rfc.zeromq.org/spec:27)
//Request:
//0. zero length frame
//1. "1.0"
//2. requestID (reply must echo)
//3. domain
//4. Origin IP address (IPv4 or IPv6 string)
//5. connectionIdentity (max 255 bytes)
//6. "CURVE"
//7. 32 byte long term public key
if(receivedContent[0] != "" || receivedContent[1] != "1.0" || receivedContent[5].size() > 255 || receivedContent[6] != "CURVE" || receivedContent[7].size() != 32 || receivedContent[2].size() < 32)
{
SOM_TRY //Send system failure message
sendReplyLambda(receivedContent[2],500); 
SOM_CATCH("Error sending reply\n")
}

if(receivedContent[2].find(receivedContent[7]) != 0)
{ //The identity does not have the key preappended, so it is invalid
SOM_TRY //Send system failure message
sendReplyLambda(receivedContent[2],400); //Authentication failure
SOM_CATCH("Error sending reply\n")
}

//Everything passed
SOM_TRY //Send system failure message
sendReplyLambda(receivedContent[2],200); //Authentication "succeeded"
SOM_CATCH("Error sending reply\n")
}

/*
This function generates the complete query string required to get all of the ids of the stations that meet the query's requirements.
@param inputRequest: This is the request to generate the query string for
@param inputParameterCountBuffer: This is set to the total number of bound variables
@return: The query string

@throws: This function can throw exceptions
*/
std::string caster::generateClientQueryRequestSQLString(const client_query_request &inputRequest, int &inputParameterCountBuffer)
{
//Construct SQL query string from client query request
std::string primaryKeyFieldName;
SOM_TRY
primaryKeyFieldName = basestationToSQLInterface->getPrimaryKeyFieldName();
SOM_CATCH("Error retrieving primary key field name\n")

//Create and store subquery clauses
int parameterCount = 0;
std::vector<std::string> subQueryStrings;
for(int i=0; i<inputRequest.subqueries_size(); i++)
{
std::string subQueryString;
if(i!=0)
{
subQueryString += " OR ";
}
subQueryString += "(";

//Handle acceptable classes subquery
subQueryString += generateInSubquery(false, "station_class", inputRequest.subqueries(i).acceptable_classes_size());
parameterCount += inputRequest.subqueries(i).acceptable_classes_size();

//Handle acceptable_formats subquery
subQueryString += generateInSubquery(parameterCount > 0, "message_format", inputRequest.subqueries(i).acceptable_formats_size());
parameterCount += inputRequest.subqueries(i).acceptable_formats_size();

for(int a=0; a<inputRequest.subqueries(i).latitude_condition_size(); a++)
{ //Handle latitude conditions
subQueryString += generateRelationalSubquery(parameterCount > 0, "latitude", inputRequest.subqueries(i).latitude_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).latitude_condition_size();

for(int a=0; a<inputRequest.subqueries(i).longitude_condition_size(); a++)
{ //Handle longitude conditions
subQueryString += generateRelationalSubquery(parameterCount > 0, "longitude", inputRequest.subqueries(i).longitude_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).longitude_condition_size();

for(int a=0; a<inputRequest.subqueries(i).uptime_condition_size(); a++)
{ //Handle uptime conditions 
subQueryString += generateRelationalSubquery(parameterCount > 0, "start_time", inputRequest.subqueries(i).uptime_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).uptime_condition_size();

for(int a=0; a<inputRequest.subqueries(i).real_update_rate_condition_size(); a++)
{ //Handle real update rate conditions
subQueryString += generateRelationalSubquery(parameterCount > 0, "real_update_rate", inputRequest.subqueries(i).real_update_rate_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).real_update_rate_condition_size();

for(int a=0; a<inputRequest.subqueries(i).expected_update_rate_condition_size(); a++)
{ //Handle real update rate conditions
subQueryString += generateRelationalSubquery(parameterCount > 0, "expected_update_rate", inputRequest.subqueries(i).expected_update_rate_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).expected_update_rate_condition_size();

if(inputRequest.subqueries(i).has_informal_name_condition())
{ //Add informal name condition
if(parameterCount > 0)
{
subQueryString += " AND ";
}

subQueryString += "(informal_name " + sqlStringRelationalOperatorToSQLString(inputRequest.subqueries(i).informal_name_condition().relation()) + "?)";
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).base_station_id_condition_size(); a++)
{ //Handle base station id conditions
subQueryString += generateRelationalSubquery(parameterCount > 0, "base_station_id", inputRequest.subqueries(i).base_station_id_condition(a).relation());
}
parameterCount += inputRequest.subqueries(i).base_station_id_condition_size();

for(int a=0; a<inputRequest.subqueries(i).source_public_keys_size(); a++)
{ //Handle source public keys restrictions
if(parameterCount > 0)
{
subQueryString += " AND ";
}

subQueryString += "(source_public_key == ?)";
}
parameterCount += inputRequest.subqueries(i).source_public_keys_size();

if(inputRequest.subqueries(i).has_circular_search_region())
{ //Handle requests for basestations within a radius of a particular location 35.779411, -78.648033
//SELECT * from coords WHERE id IN (SELECT id FROM (SELECT id, lat*lat + long*long AS distance FROM coords GROUP BY distance HAVING distance < 7740.0));
if(parameterCount > 0)
{
subQueryString += " AND ";
}
subQueryString += "(base_station_id IN (SELECT base_station_id FROM (SELECT base_station_id, (6371000*acos(cos(?)*cos(latitude)*cos(longitude-?) + sin(?)*sin(latitude))) AS distance FROM " + basestationToSQLInterface->primaryTableName + " GROUP BY distance HAVING distance <= ?)))";
//params Qlatitude, Qlongitude, Qlatitude, distance constraint value
parameterCount += 4;
}

subQueryString += ")";

if(subQueryString != " OR ()")
{ //Only add subquery if it contains clauses
subQueryStrings.push_back(subQueryString);
}

} //End subquery string generation

std::string queryString = "SELECT " + primaryKeyFieldName + " FROM " + basestationToSQLInterface->primaryTableName;

if(subQueryStrings.size() > 0)
{
queryString +=" WHERE ";
}
for(int i=0; i<subQueryStrings.size(); i++)
{
queryString += subQueryStrings[i];
}
queryString += ";";

//Can't have more than 999 bound variables
inputParameterCountBuffer = parameterCount;

return queryString;
}

/**
This function helps with creating SQL query strings for client requests.
@param inputRelation: The relation to resolve into a SQL string part (such as "<= ?"
@return: The associated SQL string component 
*/
std::string pylongps::sqlRelationalOperatorToSQLString(sql_relational_operator inputRelation)
{
if(inputRelation == LESS_THAN)
{
return "< ?";
}
else if(inputRelation == LESS_THAN_EQUAL_TO)
{
return "<= ?";
}
else if(inputRelation == EQUAL_TO)
{
return "= ?";
}
else if(inputRelation == NOT_EQUAL_TO)
{
return "!= ?";
}
else if(inputRelation == GREATER_THAN)
{
return "> ?";
}
else//(inputRelation == GREATER_THAN_EQUAL_TO)
{
return ">= ?";
}
}

/**
This function binds the fields from the client_query_request to the associated prepared statement.
@param inputStatement: The statement to bind the fields for
@param inputRequest: The request to bind the fields with
@return: The number of bound parameters

@throws: This function can throw exceptions
*/
int caster::bindClientQueryRequestFields(std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> &inputStatement, const client_query_request &inputRequest)
{
int parameterCount = 0;
for(int i=0; i<inputRequest.subqueries_size(); i++)
{
//Handle acceptable classes subquery
for(int a=0; a<inputRequest.subqueries(i).acceptable_classes_size(); a++)
{
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue((::google::protobuf::int64) inputRequest.subqueries(i).acceptable_classes(a)));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

//Handle acceptable_formats subquery
for(int a=0; a<inputRequest.subqueries(i).acceptable_formats_size(); a++)
{
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue((::google::protobuf::int64) inputRequest.subqueries(i).acceptable_formats(a)));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}


for(int a=0; a<inputRequest.subqueries(i).latitude_condition_size(); a++)
{ //Handle latitude conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).latitude_condition(a).value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).longitude_condition_size(); a++)
{ //Handle longitude conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).longitude_condition(a).value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

Poco::Timestamp currentTime;
auto timeValue = currentTime.epochMicroseconds();
for(int a=0; a<inputRequest.subqueries(i).uptime_condition_size(); a++)
{ //Handle uptime conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue((::google::protobuf::int64) (timeValue-inputRequest.subqueries(i).uptime_condition(a).value()*1000000.0)));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).real_update_rate_condition_size(); a++)
{ //Handle real_update_rate conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).real_update_rate_condition(a).value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).expected_update_rate_condition_size(); a++)
{ //Handle expected_update_rate conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).expected_update_rate_condition(a).value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

if(inputRequest.subqueries(i).has_informal_name_condition())
{ //Add informal name condition

SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).informal_name_condition().value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).base_station_id_condition_size(); a++)
{ //Handle base_station_id conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue((::google::protobuf::int64) inputRequest.subqueries(i).base_station_id_condition(a).value()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

for(int a=0; a<inputRequest.subqueries(i).source_public_keys_size(); a++)
{ //Handle source public key conditions
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).source_public_keys(a)));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}

if(inputRequest.subqueries(i).has_circular_search_region())
{ //Handle requests for basestations within a radius of a particular 
//subQueryString += "(base_station_id IN (SELECT base_station_id FROM (SELECT base_station_id, (6371000*acos(cos(?)*cos(latitude)*cos(longitude-?) + sin(?)*sin(latitude))) AS distance FROM " + basestationToSQLInterface->primaryTableName + " GROUP BY distance HAVING distance <= ?)))";
//params Qlatitude, Qlongitude, Qlatitude, distance constraint value
SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).circular_search_region().latitude()));
SOM_CATCH("Error binding statement\n")
parameterCount++;

SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).circular_search_region().longitude()));
SOM_CATCH("Error binding statement\n")
parameterCount++;

SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).circular_search_region().latitude()));
SOM_CATCH("Error binding statement\n")
parameterCount++;

SOM_TRY
basestationToSQLInterface->bindFieldToStatement(inputStatement.get(), parameterCount+1, fieldValue(inputRequest.subqueries(i).circular_search_region().radius()));
SOM_CATCH("Error binding statement\n")
parameterCount++;
}
}

return parameterCount;
}

/**
This function helps with creating SQL query strings for client requests.
@param inputStringRelation: The relation to resolve into a SQL string part (such as "LIKE ?"
@return: The associated SQL string component 
*/
std::string pylongps::sqlStringRelationalOperatorToSQLString(sql_string_relational_operator inputStringRelation)
{
if(inputStringRelation == IDENTICAL)
{
return "= ?";
}
else //LIKE
{
return "LIKE ?";
}
}

/**
This function generates a sql query subpart of the form (fieldName IN (?, ?, etc))
@param inputPreappendAND: True if an " AND " should be added before the subquery part
@param inputFieldName: The field name to have conditions on
@param inputNumberOfFields: The number of entries in the "IN" set
@return: The query subpart
*/
std::string pylongps::generateInSubquery(bool inputPreappendAND, const std::string &inputFieldName, int inputNumberOfFields)
{
std::string querySubPart;

if(inputNumberOfFields <= 0)
{
return "";
}

if(inputPreappendAND)
{
querySubPart += " AND ";
}

querySubPart += "(" + inputFieldName + "IN (";

for(int i=0; i<inputNumberOfFields; i++)
{
if(i!=0)
{
querySubPart += ", ";
}

querySubPart += "?";
}
querySubPart += "))";

return querySubPart;
}

/**
This function generates a sql query subpart of the form (fieldName IN (?, ?, etc))
@param inputPreappendAND: True if an " AND " should be added before the subquery part
@param inputFieldName: The field name to have conditions on
@param inputRelationalOperator: The relational operator to impose using the value
@return: The query subpart
*/
std::string pylongps::generateRelationalSubquery(bool inputPreappendAND, const std::string &inputFieldName, sql_relational_operator inputRelationalOperator)
{
std::string querySubPart;

if(inputPreappendAND)
{
querySubPart += " AND ";
}

querySubPart += "("+inputFieldName + " " + sqlRelationalOperatorToSQLString(inputRelationalOperator) + ")";
return querySubPart;
}

/**
This function can be used to add the "sin" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void pylongps::SQLiteSinFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues)
{
if(inputArraySize > 0)
{
sqlite3_result_double(inputContext, sin(sqlite3_value_double(inputValues[0])*DEGREES_TO_RADIANS_CONSTANT));
}
else
{
sqlite3_result_double(inputContext, 0.0);
}
}

/**
This function can be used to add the "cos" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void pylongps::SQLiteCosFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues)
{
if(inputArraySize > 0)
{
sqlite3_result_double(inputContext, cos(sqlite3_value_double(inputValues[0])*DEGREES_TO_RADIANS_CONSTANT));
}
else
{
sqlite3_result_double(inputContext, 0.0);
}
}

/**
This function can be used to add the "acos" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void pylongps::SQLiteAcosFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues)
{
if(inputArraySize > 0)
{
sqlite3_result_double(inputContext, acos(sqlite3_value_double(inputValues[0])*DEGREES_TO_RADIANS_CONSTANT));
}
else
{
sqlite3_result_double(inputContext, 0.0);
}
}

