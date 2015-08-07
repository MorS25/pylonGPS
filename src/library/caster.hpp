#ifndef  CASTERHPP
#define CASTERHPP

#include "zmq.hpp"
#include <cstdint>
#include<memory>
#include<thread>
#include<chrono>
#include<queue>
#include<string>
#include<vector>
#include<random>
#include<cmath>
#include "SOMException.hpp"
#include "SOMScopeGuard.hpp"
#include "event.hpp"
#include "utilityFunctions.hpp"
#include "Poco/Timestamp.h"
#include "protobufSQLConverter.hpp"
#include "sqlite3.h"
//#include "sqlite3ext.h"

#include "database_request.pb.h"
#include "database_reply.pb.h"
#include "client_query_request.pb.h"
#include "client_query_reply.pb.h"

namespace pylongps
{

/**
This class represents a pylonGPS 2.0 caster.  It opens several ZMQ ports to provide caster services, an in-memory SQLITE database and creates 2 threads to manage its duties.

This class/program is used to distribute information from PylonGPS Transmitters and support proxying of any and all streams via other PylonGPS Casters to enable simple scaling.  Increasing scale can be handle by having all sources point toward a central Caster and then creating other casters as proxies to disseminate the received flows.  The associated star topology should be able to serve very large numbers of receiving clients (assuming the number of clients >> the number of transmitters).  If the number of transmitters gets too large, the star topology can be fragmented so there are separate instances with each taking a fraction of the transmitters.  The in that case, the proxies simply proxy multiple Casters.  Each Caster supports requests for filtered basestation results and returns a stream ID for clients to subscribe to.  The clients can then use the ZMQ subscribe mechanism to receive updates from one or more streams.

Please see the PylonGPS 2.0 documentation and specification for more details.
*/
class caster
{
public:
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
@param inputCasterSQLITEConnectionString: The connection string used to connect to or create the SQLITE database used for stream source entry management and query resolution.  If an empty string is given (by default), it will connect/create an in memory database with a random 64 bit number string (example: "file:9735926149617295559?mode=memory&cache=shared")

@throws: This function can throw exceptions
*/
caster(zmq::context_t *inputContext, int64_t inputCasterID, uint32_t inputTransmitterRegistrationAndStreamingPortNumber, uint32_t inputAuthenticatedTransmitterRegistrationAndStreamingPortNumber, uint32_t inputClientRequestPortNumber, uint32_t inputClientStreamPublishingPortNumber, uint32_t inputProxyStreamPublishingPortNumber, uint32_t inputStreamStatusNotificationPortNumber, const std::string &inputCasterPublicKey, const std::string &inputCasterSecretKey, const std::string &inputCasterSQLITEConnectionString = "");

/**
This function signals for the threads to shut down and then waits for them to do so.
*/
~caster();

zmq::context_t *context;
int64_t casterID;
uint32_t transmitterRegistrationAndStreamingPortNumber;
uint32_t authenticatedTransmitterRegistrationAndStreamingPortNumber;
uint32_t clientRequestPortNumber;
uint32_t clientStreamPublishingPortNumber;
uint32_t proxyStreamPublishingPortNumber;
uint32_t streamStatusNotificationPortNumber;
std::string databaseConnectionString; //The connection string to use to connect to the associated SQLITE database

private:
std::string shutdownPublishingConnectionString; //string to use for inproc connection for receiving notifications for when the threads associated with this object should shut down
std::string databaseAccessConnectionString; //String to use for inproc connection to send requests to modify the database
std::string casterPublicKey;
std::string casterSecretKey;
//std::unique_ptr<Poco::Data::Session> databaseSession; //Ensures that the database doesn't go out of scope until the object does

/**
This function is called in the clientRequestHandlingThread to handle client requests and manage access to the SQLite database.
*/
void clientRequestHandlingThreadFunction();

/**
This function is called in the streamRegistrationAndPublishingThread to handle stream registration and publishing updates.
*/
void streamRegistrationAndPublishingThreadFunction();

/**
This function is called in the authenticationIDCheckingThread to verify if the ZMQ connection ID of authenticated connections matches the public key the connection is using.
*/
void authenticationIDCheckingThreadFunction();

/**
This function monitors published updates, collects the associated statistics and periodically reports them to the database.
*/
void statisticsGatheringThreadFunction();

/**
This function processes any events that are scheduled to have occurred by now and returns when the next event is scheduled to occur.  Which thread is calling this function is determined by the type of events in the event queue.
@param inputEventQueue: The event queue to process events from
@return: The time point associated with the soonest event timeout (negative if there are no outstanding events)

@throws: This function can throw exceptions
*/
Poco::Timestamp handleEvents(std::priority_queue<pylongps::event> &inputEventQueue);

//Threads/shutdown socket for operations
std::unique_ptr<zmq::socket_t> shutdownPublishingSocket; //This inproc PUB socket publishes an empty message when it is time for threads to shut down.
//Set of sockets use to listen for the signal from shutdownPublishingSocket
std::unique_ptr<zmq::socket_t> clientRequestHandlingThreadShutdownListeningSocket;
std::unique_ptr<zmq::socket_t> streamRegistrationAndPublishingThreadShutdownListeningSocket;
std::unique_ptr<zmq::socket_t> authenticationIDCheckingThreadShutdownListeningSocket;
std::unique_ptr<zmq::socket_t> statisticsGatheringThreadShutdownListeningSocket;

std::unique_ptr<zmq::socket_t> ZAPAuthenticationSocket; //A inproc REP socket that handles ID verification for the ZAP protocol (if another object has not already bound inproc://zeromq.zap.01)
std::unique_ptr<zmq::socket_t> databaseAccessSocket; //A inproc REP socket that handles requests to make changes to the database.  Used by clientRequestHandlingThread.
std::unique_ptr<std::thread> clientRequestHandlingThread; //Handles client requests and requests by the stream registration and statistics threads to make changes to the database
std::unique_ptr<std::thread> streamRegistrationAndPublishingThread;
std::unique_ptr<std::thread> authenticationIDCheckingThread; //A thread that is enabled/started to do ZMQ authentication if the inproc address "inproc://zeromq.zap.01" has not been bound already 
std::unique_ptr<std::thread> statisticsGatheringThread; //This thread analyzes the statistics of the stream messages that are published and periodically updates the associated entries in the database.

std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> databaseConnection; //Pointer to created database connection
std::unique_ptr<protobufSQLConverter<base_station_stream_information> > basestationToSQLInterface; //Allows storage/retrieval of base_station_stream_information objects in the database

//Interfaces
std::unique_ptr<zmq::socket_t> transmitterRegistrationAndStreamingInterface; ///A ZMQ ROUTER socket which expects unencrypted data (transmitter_registration_request to which it responses with a transmitter_registration_reply. If accepted, the request is followed by the data to broadcast).  Used by streamRegistrationAndPublishingThread.
std::unique_ptr<zmq::socket_t> authenticatedTransmitterRegistrationAndStreamingInterface; ///A ZMQ ROUTER socket which expects encrypted data (transmitter_registration_request with credentials to which it responses with a transmitter_registration_reply. If accepted, the request is followed by the data to broadcast) and checks the key (ZMQ connection ID must match public key).  Used by streamRegistrationAndPublishingThread.
std::unique_ptr<zmq::socket_t> clientRequestInterface;  ///A ZMQ router socket which expects a client_query_request and responds with a client_query_reply.  Used by clientRequestHandlingThread.
std::unique_ptr<zmq::socket_t> clientStreamPublishingInterface; ///A ZMQ PUB socket which publishes all data associated with all streams with the caster ID and stream ID preappended for clients to subscribe.  Used by streamRegistrationAndPublishingThread.
std::unique_ptr<zmq::socket_t> proxyStreamPublishingInterface; ///A ZMQ PUB socket which publishes all data associated with all streams with the caster ID and stream ID preappended for clients to subscribe.  Used by streamRegistrationAndPublishingThread.
std::unique_ptr<zmq::socket_t> streamStatusNotificationInterface; ///A ZMQ PUB socket which publishes stream_status_update messages.  Used by streamRegistrationAndPublishingThread.

//Functions and objects used internally
/**
This function sets up the basestationToSQLInterface and generates the associated tables so that basestations can be stored and returned.  databaseConnection must be setup before this function is called.

@throws: This function can throw exceptions
*/
void setupBaseStationToSQLInterface();

/**
This function checks if the databaseAccessSocket has received a database_request message and (if so) processes the message and sends a database_reply in response.

@throws: This function can throw exceptions
*/
void processDatabaseRequest();

/**
This function checks if the clientRequestInterface has received a client_query_request message and (if so) processes the message and sends a client_query_reply in response.

@throws: This function can throw exceptions
*/
void processClientQueryRequest();

/**
This process checks if ZAPAuthenticationSocket has received a ZAP request.  If so, it checks to make sure that the first 32 bytes of the connection identity contains the public key in the credentials, so that the key associated with a connection can be checked using the identity at the authenticatedTransmitterRegistrationAndStreamingInterface using a credentials message sent over that connection

@throws: This function can throw exceptions
*/
void processZAPAuthenticationRequest();

/**
This function generates the complete query string required to get all of the ids of the stations that meet the query's requirements.
@param inputRequest: This is the request to generate the query string for
@param inputParameterCountBuffer: This is set to the total number of bound variables
@return: The query string

@throws: This function can throw exceptions
*/
std::string generateClientQueryRequestSQLString(const client_query_request &inputRequest, int &inputParameterCountBuffer);

/**
This function binds the fields from the client_query_request to the associated prepared statement.
@param inputStatement: The statement to bind the fields for
@param inputRequest: The request to bind the fields with
@return: The number of bound parameters

@throws: This function can throw exceptions
*/
int bindClientQueryRequestFields(std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> &inputStatement, const client_query_request &inputRequest);

};



/**
This function helps with creating SQL query strings for client requests.
@param inputRelation: The relation to resolve into a SQL string part (such as "<= ?"
@return: The associated SQL string component 
*/
std::string sqlRelationalOperatorToSQLString(sql_relational_operator inputRelation);


/**
This function helps with creating SQL query strings for client requests.
@param inputStringRelation: The relation to resolve into a SQL string part (such as "LIKE ?"
@return: The associated SQL string component 
*/
std::string sqlStringRelationalOperatorToSQLString(sql_string_relational_operator inputStringRelation);

/**
This function generates a sql query subpart of the form (fieldName IN (?, ?, etc))
@param inputPreappendAND: True if an " AND " should be added before the subquery part
@param inputFieldName: The field name to have conditions on
@param inputNumberOfFields: The number of entries in the "IN" set
@return: The query subpart
*/
std::string generateInSubquery(bool inputPreappendAND, const std::string &inputFieldName, int inputNumberOfFields);

/**
This function generates a sql query subpart of the form (fieldName IN (?, ?, etc))
@param inputPreappendAND: True if an " AND " should be added before the subquery part
@param inputFieldName: The field name to have conditions on
@param inputRelationalOperator: The relational operator to impose using the value
@return: The query subpart
*/
std::string generateRelationalSubquery(bool inputPreappendAND, const std::string &inputFieldName, sql_relational_operator inputRelationalOperator);

//SQLITE functions to add for support of great circle calculations
const double PI = 3.14159265359;
const double DEGREES_TO_RADIANS_CONSTANT = PI/180.0;

/**
This function can be used to add the "sin" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void SQLiteSinFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues);

/**
This function can be used to add the "cos" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void SQLiteCosFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues);

/**
This function can be used to add the "acos" function to the current SQLite connection.
@param inputContext: The current SQLite context
@param inputArraySize: The number of values in the array
@param inputValues: The array values
*/
void SQLiteAcosFunctionDegrees(sqlite3_context *inputContext, int inputArraySize, sqlite3_value **inputValues);





}
#endif
