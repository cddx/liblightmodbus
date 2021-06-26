#include "master.h"
#include "master_func.h"

ModbusMasterFunctionHandler modbusMasterDefaultFunctions[] =
{
#ifdef LIGHTMODBUS_F01M
	{1, modbusParseResponse01020304},
#endif

#ifdef LIGHTMODBUS_F02M
	{2, modbusParseResponse01020304},
#endif

#ifdef LIGHTMODBUS_F03M
	{3, modbusParseResponse01020304},
#endif

#ifdef LIGHTMODBUS_F04M
	{4, modbusParseResponse01020304},
#endif

#ifdef LIGHTMODBUS_F05M
	{5, modbusParseResponse0506},
#endif

#ifdef LIGHTMODBUS_F06M
	{6, modbusParseResponse0506},
#endif

#ifdef LIGHTMODBUS_F15M
	{15, modbusParseResponse1516},
#endif

#ifdef LIGHTMODBUS_F16M
	{16, modbusParseResponse1516},
#endif

#ifdef LIGHTMODBUS_F22M
	{22, modbusParseResponse22},
#endif
};

/**
	\brief Default allocator for master device. Based on modbusDefaultAllocator().
	\param ptr pointer to the pointer to the buffer
	\param size 
	\returns \ref MODBUS_ERROR_ALLOC on allocation failure
*/
LIGHTMODBUS_RET_ERROR modbusMasterDefaultAllocator(ModbusMaster *status, uint8_t **ptr, uint16_t size, ModbusBufferPurpose purpose)
{
	return modbusDefaultAllocator(ptr, size, purpose);
}

/**
	\brief Allocates memory for the request frame
	\param pdusize size of the PDU section of the frame. 0 implies no request at all.
	\returns \ref MODBUS_ERROR_ALLOC on allocation failure

	If called with size == 0, the request buffer is freed. Otherwise a buffer
	for `(pdusize + status->request.padding)` bytes is allocated. This guarantees
	that if a response is made, the buffer is big enough to hold the entire ADU.

	This function is responsible for managing `data`, `pdu` and `length` fields
	in the request struct. The `pdu` pointer is set up to point `pduOffset` bytes
	after the `data` pointer unless `data` is a null pointer.
*/
LIGHTMODBUS_RET_ERROR modbusMasterAllocateRequest(
	ModbusMaster *status,
	uint16_t pdusize)
{
	uint16_t size = pdusize;
	if (pdusize) size += status->request.padding;

	ModbusError err = status->allocator(
		status,
		&status->request.data,
		size,
		MODBUS_MASTER_REQUEST_BUFFER);

	if (err == MODBUS_ERROR_ALLOC || size == 0)
	{
		status->request.data = NULL;
		status->request.pdu = NULL;
		status->request.length = 0;
	}
	else
	{
		status->request.pdu = status->request.data + status->request.pduOffset;
		status->request.length = size;
	}
	
	return err;
}

/**
	\brief Initializes a ModbusMaster struct
	\param status ModbusMaster struct to be initialized
	\param allocator Memory allocator to be used (see \ref modbusMasterDefaultAllocator) (required)
	\param dataCallback Callback function for handling incoming data (required)
	\param exceptionCallback Callback function for handling slave exceptions (optional)

	\see modbusSlaveDefaultAllocator()
	\see modbusMasterDefaultFunctions
*/
LIGHTMODBUS_RET_ERROR modbusMasterInit(
	ModbusMaster *status,
	ModbusMasterAllocator allocator,
	ModbusDataCallback dataCallback,
	ModbusMasterExceptionCallback exceptionCallback)
{
	status->allocator = allocator;
	status->dataCallback = dataCallback;
	status->exceptionCallback = exceptionCallback;

	status->functions = modbusMasterDefaultFunctions;
	status->functionCount = MODBUS_MASTER_DEFAULT_FUNCTION_COUNT;

	status->context = NULL;
	status->request.data = NULL;
	status->request.pdu = NULL;
	status->request.length = 0;
	status->request.padding = 0;
	status->request.pduOffset = 0;

	return MODBUS_OK;
}

/**
	\brief Deinitializes a ModbusMaster struct
*/
void modbusMasterDestroy(ModbusMaster *status)
{
	(void) modbusMasterAllocateRequest(status, 0);
}

/**
	\brief Begins a PDU-only request
*/
ModbusMaster *modbusBeginRequestPDU(ModbusMaster *status)
{
	status->request.pduOffset = 0;
	status->request.padding = 0;
	return status;
}

/**
	\brief Finalizes a PDU-only request
	\param err Used for error propagation from modbusBuildRequestxx
	\returns Propagated error value
*/
LIGHTMODBUS_RET_ERROR modbusEndRequestPDU(ModbusMaster *status, ModbusError err)
{
	return err;
}

/**
	\brief Begins a RTU request
*/
ModbusMaster *modbusBeginRequestRTU(ModbusMaster *status)
{
	status->request.pduOffset = 1;
	status->request.padding = 3;
	return status;
}

/**
	\brief Finalizes a Modbus RTU request
	\param err Used for error propagation from modbusBuildRequestxx
	\returns Propagated error value if non-zero
	\returns \ref MODBUS_ERROR_LENGTH if the allocated frame is too short 
*/
LIGHTMODBUS_RET_ERROR modbusEndRequestRTU(ModbusMaster *status, uint8_t address, ModbusError err)
{
	if (err) return err;
	if (status->request.length < 4) return MODBUS_ERROR_LENGTH;

	// Put in slave address
	status->request.data[0] = address;

	// Compute and put in CRC
	uint16_t crc = modbusCRC(&status->request.data[0], status->request.length - 2);
	modbusWLE(&status->request.data[status->request.length - 2], crc);

	return MODBUS_OK;
}

/**
	\brief Begins a TCP request
*/
ModbusMaster *modbusBeginRequestTCP(ModbusMaster *status)
{
	status->request.pduOffset = 0;
	status->request.padding = 7;
	return status;
}

/**
	\brief Finalizes a Modbus TCP request
	\param err Used for error propagation from modbusBuildRequestxx
	\returns Propagated error value if non-zero
	\returns \ref MODBUS_ERROR_LENGTH if the allocated frame is too short 
*/
LIGHTMODBUS_RET_ERROR modbusEndRequestTCP(ModbusMaster *status, uint16_t transaction, uint8_t unit, ModbusError err)
{
	if (err) return err;
	if (status->request.length < 7) return MODBUS_ERROR_LENGTH;

	uint16_t length = status->request.length - 6;
	modbusWBE(&status->request.data[0], transaction); // Transaction ID
	modbusWBE(&status->request.data[2], 0);           // Protocol ID
	modbusWBE(&status->request.data[4], length);      // Data length
	status->request.data[6] = unit;                   // Unit ID

	return MODBUS_OK;
}

/**
	\brief Parses a PDU section of a slave response
	\param address Address of the slave that sent in the data
	\param request Pointer to the PDU section of the request frame
	\param requestLength Length of the request PDU
	\param response Pointer to the PDU section of the response
	\param responseLength Length of the response PDU
	\returns Result from the parsing function on success
	\returns \ref MODBUS_ERROR_FUNCTION if the function code in request doesn't match the one in response
	\returns \ref MODBUS_ERROR_FUNCTION if the function is not supported
	\returns \ref MODBUS_ERROR_LENGTH if either the request or response has zero length
*/
LIGHTMODBUS_RET_ERROR modbusParseResponsePDU(
	ModbusMaster *status,
	uint8_t address,
	const uint8_t *request,
	uint8_t requestLength,
	const uint8_t *response,
	uint8_t responseLength)
{
	// Check if lengths are ok
	if (!requestLength || !responseLength)
		return MODBUS_ERROR_LENGTH;

	uint8_t function = response[0];

	// Handle exception frames
	if (function & 0x80 && responseLength == 2)
	{
		if (status->exceptionCallback)
			status->exceptionCallback(
				status,
				address,
				function & 0x7f,
				(ModbusExceptionCode) response[1]);

		return MODBUS_OK;
	}

	// Check if function code matches the one in request frame
	if (function != request[0])
		return MODBUS_ERROR_FUNCTION;

	// Find a parsing function
	for (uint16_t i = 0; i < status->functionCount; i++)
		if (function == status->functions[i].id)
			return status->functions[i].ptr(
				status,
				function,
				request,
				requestLength,
				response,
				responseLength);

	// No matching function handler
	return MODBUS_ERROR_FUNCTION;
}

/**
	\brief Parses a Modbus RTU slave response
	\param request Pointer to the request frame
	\param requestLength Length of the request
	\param response Pointer to the response frame
	\param responseLength Length of the response
	\returns Result of \ref modbusParseResponsePDU() if the PDU extraction was successful
	\returns \ref MODBUS_ERROR_CRC if the frame CRC is invalid
	\returns \ref MODBUS_ERROR_ADDRESS if the address is 0 or request/response addresess don't match
	\returns \ref MODBUS_ERROR_LENGTH if request or response is too short

	\todo Consider omitting CRC for request for better perf?
*/
LIGHTMODBUS_RET_ERROR modbusParseResponseRTU(
	ModbusMaster *status,
	const uint8_t *request,
	uint16_t requestLength,
	const uint8_t *response,
	uint16_t responseLength)
{
	// Check lengths
	if (requestLength < 4 || responseLength < 4)
		return MODBUS_ERROR_LENGTH;

	// Check CRC
	uint16_t requestCRC = modbusCRC(request, requestLength - 2);
	if (requestCRC != modbusRLE(&request[requestLength - 2]))
		return MODBUS_ERROR_CRC;
	uint16_t responseCRC = modbusCRC(response, responseLength - 2);
	if (responseCRC != modbusRLE(&response[responseLength - 2]))
		return MODBUS_ERROR_CRC;

	// Check addresses
	uint8_t address = request[0];
	if (address == 0 || request[0] != response[0])
		return MODBUS_ERROR_ADDRESS;

	return modbusParseResponsePDU(
		status,
		address,
		request + 1,
		requestLength - 3,
		response + 1,
		responseLength - 3);
}

/**
	\brief Parses a Modbus TCP slave response
	\param request Pointer to the request frame
	\param requestLength Length of the request
	\param response Pointer to the response frame
	\param responseLength Length of the response
	\returns Result of \ref modbusParseResponsePDU() if the PDU extraction was successful
	\returns \ref MODBUS_ERROR_LENGTH if request or response is too short
	\returns \ref MODBUS_ERROR_BAD_PROTOCOL if the Protocol ID field is non-zero
	\returns \ref MODBUS_ERROR_BAD_TRANSACTION if the request and response transaction IDs don't match
	\returns \ref MODBUS_ERROR_LENGTH if the request/response lengths don't match the declared ones

	\todo Consider omitting CRC for request for better perf?
*/
LIGHTMODBUS_RET_ERROR modbusParseResponseTCP(
	ModbusMaster *status,
	const uint8_t *request,
	uint16_t requestLength,
	const uint8_t *response,
	uint16_t responseLength)
{
	// Check lengths
	if (requestLength < 8 || responseLength < 8)
		return MODBUS_ERROR_LENGTH;

	// Check if protocol IDs are correct
	if (modbusRBE(&request[2]) != 0 || modbusRBE(&response[0]) != 0)
		return MODBUS_ERROR_BAD_PROTOCOL;

	// Check if transaction IDs match
	if (modbusRBE(&request[0]) != modbusRBE(&response[0]))
		return MODBUS_ERROR_BAD_TRANSACTION;

	// Check if lengths are ok
	if (modbusRBE(&request[4]) != requestLength - 6
		|| modbusRBE(&response[4]) != responseLength - 6)
		return MODBUS_ERROR_LENGTH;

	//! \todo Check addresses (but how?)
	uint8_t address = response[6];

	return modbusParseResponsePDU(
		status,
		address,
		request + 7,
		requestLength - 7,
		response + 7,
		responseLength - 7);
}