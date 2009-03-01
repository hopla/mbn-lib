
#include <stdlib.h>
#include <string.h>

#include "mbn.h"
#include "codec.h"


/* Converts 7bits data to 8bits. The result buffer must
 * be at least 8/7 times as large as the input buffer.
 * (shamelessly stolen from Anton's Decode7to8bits()) */
int convert_7to8bits(unsigned char *buffer, unsigned char length, unsigned char *result) {
  int i, reslength = 0;
  unsigned char mask1, mask2;

  result[reslength] = buffer[0]&0x7F;
  for(i=1; i<length; i++) {
    mask1 = (0x7F>>(i&0x07))<<(i&0x07);
    mask2 = mask1^0x7F;

    if(mask2 != 0x00)
      result[reslength++] |= (buffer[i] & mask2) << (8 - (i & 0x07));
    result[reslength] = (buffer[i] & mask1) >> (i & 0x07);
  }

  return reslength;
}


/* Converts variable float into the native float type
 * of the current CPU. Returns non-zero on failure.
 * (shamelessly stolen from Anton's VariableFloat2Float())
 * Note: this function does assume that the CPU
 *  represenation of the float type is a 32 bits
 *  IEEE 754, but we're probably quite safe with that */
int convert_varfloat_to_float(unsigned char *buffer, unsigned char length, float *result) {
  unsigned long tmp;
  int exponent;
  unsigned long mantessa;
  char signbit;

  /* check length */
  if(length == 0 || length == 3 || length > 4)
    return 1;

  /* check for zero (is this even necessary?) */
  tmp = 0x00000000;
  if(memcmp((void *)&tmp, (void *)buffer, length) == 0) {
    *result = *((float *)&tmp);
    return 0;
  }

  /* otherwise, calculate float */
  switch(length) {
    case 1:
      signbit  = (buffer[0]>>7)&0x01;
      exponent = (buffer[0]>>4)&0x07;
      mantessa = (buffer[0]   )&0x0F;
      if(exponent == 0) /* denormalized */
        exponent = -127;
      else if(exponent == 7) /* +/-INF or NaN, depends on sign */
        exponent = 128;
      else
        exponent -= 3;

      tmp = (signbit<<31) | (((exponent+127)&0xFF)<<23) | ((mantessa&0x0F)<<19);
      *result = *((float *)&tmp);
      break;
    case 2:
      signbit  = (buffer[0]>>7)&0x01;
      exponent = (buffer[0]>>2)&0x1F;
      mantessa = (((unsigned long) buffer[0]&0x03)<<8) | (buffer[1]&0xFF);
      if(exponent == 0) /* denormalized */
        exponent = -127;
      else if(exponent == 31) /* +/-INF, NaN, depends on sign */
        exponent = 128;
      else
        exponent -= 15;

      tmp = (signbit<<31) | (((exponent+127)&0xFF)<<23) | (((mantessa>>8)&0x03)<<21) | ((mantessa&0xFF)<<13);
      *result = *((float *)&tmp);
      break;
    case 4:
      tmp = (buffer[0]<<24) | (buffer[1]<<16) | (buffer[2]<<8) | buffer[3];
      *result = *((float *)&tmp);
      break;
  }

  return 0;
}


/* Parses the data part of Address Reservation Messages,
 * returns non-zero on failure */
int parsemsg_address(struct mbn_message *msg) {
  struct mbn_message_address *addr = &(msg->Data.Address);

  if(msg->bufferlength != 16)
    return 1;
  addr->Type = msg->buffer[0];
  addr->ManufacturerID     = ((unsigned short) msg->buffer[ 1]<< 8) | (unsigned short) msg->buffer[ 2];
  addr->ProductID          = ((unsigned short) msg->buffer[ 3]<< 8) | (unsigned short) msg->buffer[ 4];
  addr->UniqueIDPerProduct = ((unsigned short) msg->buffer[ 5]<< 8) | (unsigned short) msg->buffer[ 6];
  addr->MambaNetAddr       = ((unsigned long)  msg->buffer[ 7]<<24) |((unsigned long)  msg->buffer[ 8]<<16)
                           | ((unsigned long)  msg->buffer[ 9]<< 8) | (unsigned long)  msg->buffer[10];
  addr->EngineAddr         = ((unsigned long)  msg->buffer[11]<<24) |((unsigned long)  msg->buffer[12]<<16)
                           | ((unsigned long)  msg->buffer[13]<< 8) | (unsigned long)  msg->buffer[14];
  addr->Services = msg->buffer[15];
  return 0;
}


/* Converts a data type into a union, allocating memory for the
 * types that need it. Returns non-zero on failure. */
int parse_datatype(unsigned char type, unsigned char *buffer, int length, union mbn_message_object_data *result) {
  struct mbn_message_object_information *nfo;
  int i;

  switch(type) {
    case MBN_DATATYPE_NODATA:
      if(length > 0)
        return 1;
      break;

    case MBN_DATATYPE_UINT:
    case MBN_DATATYPE_STATE:
      if(length < 1 || length > 4)
        return 1;
      result->UInt = 0;
      for(i=0; i<length; i++) {
        result->UInt <<= 8;
        result->UInt |= buffer[i];
      }
      if(type == MBN_DATATYPE_STATE)
        result->State = result->UInt;
      break;

    case MBN_DATATYPE_SINT:
      if(length < 1 || length > 4)
        return 1;
      /* parse it as a normal unsigned int */
      for(i=0; i<length; i++) {
        result->UInt <<= 8;
        result->UInt |= buffer[i];
      }
      /* check for sign bit, and set the MSB bits to 1 */
      /* This trick assumes that signed long types are always
       * two's complement and 32 bits.
       * TODO: test this on a 64bit cpu */
      if(buffer[0] & 0x80) {
        result->UInt |= length == 1 ? 0xFFFFFF80
                      : length == 2 ? 0xFFFF8000
                      : length == 3 ? 0xFF800000
                      :               0x80000000;
      }
      /* this shouldn't be necessary, but does guarantee portability */
      memmove((void *)&(result->SInt), (void *)&(result->UInt), sizeof(result->SInt));
      break;

    case MBN_DATATYPE_OCTETS:
    case MBN_DATATYPE_ERROR:
      if((type == MBN_DATATYPE_OCTETS && length < 1) || length > 64)
        return 1;
      /* Note: we add an extra \0 to the octets so using string functions won't
       * trash the application. The MambaNet protocol doesn't require this. */
      result->Octets = malloc(length+1);
      memcpy(result->Octets, buffer, length);
      result->Octets[length] = 0;
      if(type == MBN_DATATYPE_ERROR)
        result->Error = result->Octets;
      break;

    case MBN_DATATYPE_FLOAT:
      if(convert_varfloat_to_float(buffer, length, &(result->Float)) != 0)
        return 1;
      break;

    case MBN_DATATYPE_BITS:
      if(length < 1 || length > 8)
        return 1;
      memcpy(result->Bits, buffer, length);
      break;

    case MBN_DATATYPE_OBJINFO:
      if(length < 37 || length > 77)
        return 1;
      nfo = (struct mbn_message_object_information *) calloc(1, sizeof(struct mbn_message_object_information));
      i = 32;
      memcpy(nfo->Description, buffer, i);
      nfo->Services = buffer[i++];
      nfo->SensorType = buffer[i++];
      nfo->SensorSize = buffer[i++];
      if((nfo->SensorSize*2)+i > length) {
        free(nfo);
        return 4;
      }
      if(parse_datatype(nfo->SensorType, &(buffer[i]), nfo->SensorSize, &(nfo->SensorMin)) != 0) {
        free(nfo);
        return 3;
      }
      i += nfo->SensorSize;
      if(parse_datatype(nfo->SensorType, &(buffer[i]), nfo->SensorSize, &(nfo->SensorMax)) != 0) {
        free(nfo);
        return 3;
      }
      i += nfo->SensorSize;
      nfo->ActuatorType = buffer[i++];
      nfo->ActuatorSize = buffer[i++];
      if((nfo->ActuatorSize*3)+i > length) {
        free(nfo);
        return 4;
      }
      if(parse_datatype(nfo->ActuatorType, &(buffer[i]), nfo->ActuatorSize, &(nfo->ActuatorMin)) != 0) {
        free(nfo);
        return 3;
      }
      i += nfo->ActuatorSize;
      if(parse_datatype(nfo->ActuatorType, &(buffer[i]), nfo->ActuatorSize, &(nfo->ActuatorMax)) != 0) {
        free(nfo);
        return 3;
      }
      i += nfo->SensorSize;
      if(parse_datatype(nfo->ActuatorType, &(buffer[i]), nfo->ActuatorSize, &(nfo->ActuatorDefault)) != 0) {
        free(nfo);
        return 3;
      }
      break;

    default:
      return 2;
  }
  return 0;
}


/* Parses the data part of Object Messages,
 * returns non-zero on failure */
int parsemsg_object(struct mbn_message *msg) {
  int r;
  struct mbn_message_object *obj = &(msg->Data.Object);

  if(msg->bufferlength < 4)
    return 1;
  /* header */
  obj->Number   = ((unsigned short) msg->buffer[0]<<8) | (unsigned short) msg->buffer[1];
  obj->Action   = msg->buffer[2];
  obj->DataType = msg->buffer[3];
  obj->DataSize = 0;

  /* No data? stop processing */
  if(obj->DataType == MBN_DATATYPE_NODATA) {
    if(msg->bufferlength > 4)
      return 2;
    return 0;
  }

  /* Data, so parse it */
  obj->DataSize = msg->buffer[4];
  if(obj->DataSize != msg->bufferlength-5)
    return 3;

  if((r = parse_datatype(obj->DataType, msg->buffer, obj->DataSize, &(obj->Data))) != 0)
    return r | 8;

  return 0;
}


/* Parses a raw MambaNet message and puts the results back in the struct,
 *  allocating memory where necessary.
 * returns non-zero on failure */
int parse_message(struct mbn_message *msg) {
  int l, err;

  /* Message is too small for a header to fit */
  if(msg->rawlength < 15)
    return 0x01;

  /* decode MambaNet header */
  msg->ControlByte = msg->raw[0];
  msg->AddressTo    = ((unsigned long)  msg->raw[ 0]<<28) & 0x10000000;
  msg->AddressTo   |= ((unsigned long)  msg->raw[ 1]<<21) & 0x0FE00000;
  msg->AddressTo   |= ((unsigned long)  msg->raw[ 2]<<14) & 0x001FC000;
  msg->AddressTo   |= ((unsigned long)  msg->raw[ 3]<< 7) & 0x00003F80;
  msg->AddressTo   |= ((unsigned long)  msg->raw[ 4]    ) & 0x0000007F;
  msg->AddressFrom  = ((unsigned long)  msg->raw[ 5]<<21) & 0x0FE00000;
  msg->AddressFrom |= ((unsigned long)  msg->raw[ 6]<<14) & 0x001FC000;
  msg->AddressFrom |= ((unsigned long)  msg->raw[ 7]<< 7) & 0x00003F80;
  msg->AddressFrom |= ((unsigned long)  msg->raw[ 8]    ) & 0x0000007F;
  msg->MessageID    = ((unsigned long)  msg->raw[ 9]<<14) & 0x001FC000;
  msg->MessageID   |= ((unsigned long)  msg->raw[10]<< 7) & 0x00003F80;
  msg->MessageID   |= ((unsigned long)  msg->raw[11]    ) & 0x0000007F;
  msg->MessageType  = ((unsigned short) msg->raw[12]<< 7) &     0x3F80;
  msg->MessageType |= ((unsigned short) msg->raw[13]    ) &     0x007F;
  msg->DataLength   = ((unsigned char)  msg->raw[14]    ) &       0x7F;

  /* done parsing if there's no data */
  if(msg->DataLength == 0)
    return 0;

  /* check for the validness of the DataLength */
  for(l=0; msg->raw[l+15] != 0xFF && l+15 < msg->rawlength; l++)
    ;
  if(msg->DataLength != l)
    return 0x03;

  /* fill the 8bit buffer */
  msg->bufferlength = convert_7to8bits(&(msg->raw[15]), msg->DataLength, msg->buffer);

  /* parse the data part */
  if(msg->MessageType == MBN_MSGTYPE_ADDRESS) {
    if((err = parsemsg_address(msg)) != 0)
      return err | 0x10;
  } else if(msg->MessageType == MBN_MSGTYPE_OBJECT) {
    if((err = parsemsg_object(msg)) != 0)
      return err | 0x20;
  }

  return 0;
}

