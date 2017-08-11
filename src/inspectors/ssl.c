#include "inspectors.h"
#include "../api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DPI_DEBUG_SSL 0
#define debug_print(fmt, ...) \
            do { if (DPI_DEBUG_SSL) fprintf(stdout, fmt, __VA_ARGS__); } while (0)

#define ndpi_isalpha(ch) (((ch) >= 'a' && (ch) <= 'z') || ((ch) >= 'A' && (ch) <= 'Z'))
#define ndpi_isdigit(ch) ((ch) >= '0' && (ch) <= '9')
#define ndpi_isspace(ch) (((ch) >= '\t' && (ch) <= '\r') || ((ch) == ' '))
#define ndpi_isprint(ch) ((ch) >= 0x20 && (ch) <= 0x7e)
#define ndpi_ispunct(ch) (((ch) >= '!' && (ch) <= '/') ||	\
			  ((ch) >= ':' && (ch) <= '@') ||	\
			  ((ch) >= '[' && (ch) <= '`') ||	\
			  ((ch) >= '{' && (ch) <= '~'))

u_int8_t dpi_ssl_activate_callbacks(
		       dpi_library_state_t* state,
		       dpi_ssl_callbacks_t* callbacks,
		       void* user_data)
{
	if(state){
		BITSET(state->tcp_protocols_to_inspect, DPI_PROTOCOL_TCP_SSL);
		BITSET(state->tcp_active_callbacks, DPI_PROTOCOL_TCP_SSL);
		state->ssl_callbacks_user_data=user_data;
		state->ssl_callbacks=callbacks;
		return DPI_STATE_UPDATE_SUCCESS;
	}else{
		return DPI_STATE_UPDATE_FAILURE;
	}
}

u_int8_t dpi_ssl_disable_callbacks(dpi_library_state_t* state)
{
	if(state){
		BITCLEAR(state->tcp_active_callbacks, DPI_PROTOCOL_TCP_SSL);
		state->ssl_callbacks=NULL;
		state->ssl_callbacks_user_data=NULL;
		return DPI_STATE_UPDATE_SUCCESS;
	}else{
		return DPI_STATE_UPDATE_FAILURE;
	}
}


/* implementation of the punycode check function */
static int check_punycode_string(char * buffer , int len)
{
	int i = 0;

	while(i++ < len)
	{
		if( buffer[i] == 'x' && buffer[i+1] == 'n' && buffer[i+2] == '-' && buffer[i+3] == '-' )
			return 1;
	}
	// not a punycode string
	return 0;
}

static void stripCertificateTrailer(char *buffer, int buffer_len)
{
	int i, is_puny;
	for(i = 0; i < buffer_len; i++)
	{
		if((buffer[i] != '.')
		&& (buffer[i] != '-')
		&& (buffer[i] != '_')
		&& (buffer[i] != '*')
		&& (!ndpi_isalpha(buffer[i]))
		&& (!ndpi_isdigit(buffer[i])))
		{
			buffer[i] = '\0';
			buffer_len = i;
			break;
		}
	}

	/* check for punycode encoding */
	is_puny = check_punycode_string(buffer, buffer_len);

	// not a punycode string - need more checks
	if(is_puny == 0)
	{
		if(i > 0)
			i--;
		while(i > 0)
		{
			if(!ndpi_isalpha(buffer[i]))
			{
				buffer[i] = '\0';
				buffer_len = i;
				i--;
			} else
				break;
		}
		for(i = buffer_len; i > 0; i--)
		{
			if(buffer[i] == '.')
				break;
			else if(ndpi_isdigit(buffer[i]))
				buffer[i] = '\0', buffer_len = i;
		}
	}
}

static int getSSLcertificate(uint8_t *payload, u_int payload_len, dpi_ssl_internal_information_t *t, dpi_pkt_infos_t* pkt)
{
	if(payload[0] == 0x16 /* Handshake */)
	{
		u_int16_t total_len  = (payload[3] << 8) + payload[4] + 5 /* SSL Header */;
		u_int8_t handshake_protocol = payload[5]; /* handshake protocol a bit misleading, it is message type according TLS specs */

		if(total_len <= 4)
			return 0;

		if(total_len > payload_len)
		{
			if(handshake_protocol == 0x01)
			{
				return 3; // need more data
			} else {
				return 0;
			}
		}


		int i;
		if(handshake_protocol == 0x02 || handshake_protocol == 0xb)
		{
			u_int num_found = 0;
			// Check after handshake protocol header (5 bytes) and message header (4 bytes)
			for(i = 9; i < payload_len-3; i++)
			{
				if(((payload[i] == 0x04) && (payload[i+1] == 0x03) && (payload[i+2] == 0x0c))
				|| ((payload[i] == 0x04) && (payload[i+1] == 0x03) && (payload[i+2] == 0x13))
				|| ((payload[i] == 0x55) && (payload[i+1] == 0x04) && (payload[i+2] == 0x03)))
				{
					u_int8_t server_len = payload[i+3];
					if(payload[i] == 0x55)
					{
						num_found++;
						if(num_found != 2)
							continue;
					}
					if(server_len+i+3 < payload_len)
					{
						char *server_name = (char*)&payload[i+4];
						u_int8_t begin = 0, j, num_dots, len;
						while(begin < server_len)
						{
							if(!ndpi_isprint(server_name[begin]))
								begin++;
							else
								break;
						}
						len = server_len - begin;
						for(j=begin, num_dots = 0; j<len; j++)
						{
							if(!ndpi_isprint((server_name[j])))
							{
								num_dots = 0; // This is not what we look for
								break;
							} else if(server_name[j] == '.')
							{
								num_dots++;
								if(num_dots >=2)
									break;
							}
						}
						if(num_dots >= 2)
						{
							if(t->callbacks != NULL && t->callbacks->certificate_callback != NULL && len > 0)
							{
								(*(t->callbacks->certificate_callback))(&server_name[begin], len, t->callbacks_user_data, pkt);
							}
							return 2;
						}
					}
				}
			}
			return 4;
		} else if(handshake_protocol == 0x01 )
		{
			u_int offset, base_offset = 43;
			if (base_offset + 2 <= payload_len)
			{
				u_int16_t session_id_len = payload[base_offset];
				if((session_id_len+base_offset+2) <= total_len)
				{
					u_int16_t cypher_len =  payload[session_id_len+base_offset+2] + (payload[session_id_len+base_offset+1] << 8);
					offset = base_offset + session_id_len + cypher_len + 2;
					if(offset < total_len)
					{
						u_int16_t compression_len;
						u_int16_t extensions_len;
						compression_len = payload[offset+1];
						offset += compression_len + 3;
						if(offset < total_len)
						{
							extensions_len = payload[offset];
							if((extensions_len+offset) < total_len)
							{
								/* Move to the first extension
								Type is u_int to avoid possible overflow on extension_len addition */
								u_int extension_offset = 1;
								while(extension_offset < extensions_len)
								{
									uint16_t extension_id = (payload[offset+extension_offset] << 8) + payload[offset+extension_offset+1];
									extension_offset += 2;
									uint16_t extension_len = (payload[offset+extension_offset] << 8) + payload[offset+extension_offset+1];
									extension_offset += 2;
									if(extension_len > total_len) /* bad ssl */
										return 0;
									if(extension_id == 0)
									{
										u_int begin = 0,len;
										char *server_name = (char*)&payload[offset+extension_offset];
										if(payload[offset+extension_offset+2] == 0x00) // host_name
											begin =+ 5;
										while(begin < extension_len)
										{
											if((!ndpi_isprint(server_name[begin])) || ndpi_ispunct(server_name[begin]) || ndpi_isspace(server_name[begin]))
												begin++;
											else
												break;
										}
										len = extension_len - begin;
										if(len > total_len)
										{
											return 0; /* bad ssl */
										}
										if(t->callbacks != NULL && t->callbacks->certificate_callback != NULL && len > 0)
										{
											(*(t->callbacks->certificate_callback))(&server_name[begin], len, t->callbacks_user_data, pkt);
										}
										return 2;
									}
									extension_offset += extension_len;
								}
								return 4; // SSL, but no certificate
							}
						}
					}
				}
			}
		}
	}
	return 0;
}

static int detectSSLFromCertificate(uint8_t *payload, int payload_len, dpi_ssl_internal_information_t *t, dpi_pkt_infos_t* pkt)
{
	if((payload_len > 9) && (payload[0] == 0x16 /* consider only specific SSL packets (handshake) */))
	{
		int rc = getSSLcertificate(payload, payload_len, t, pkt);
		if(rc > 0)
		{
			return rc;
		}
	}
	return 0;
}

u_int8_t invoke_callbacks_ssl(dpi_library_state_t* state, dpi_pkt_infos_t* pkt, const unsigned char* app_data, u_int32_t data_length, dpi_tracking_informations_t* tracking)
{
	debug_print("%s\n", "[ssl.c] SSL callback manager invoked.");
	u_int8_t ret=check_ssl(state, pkt, app_data, data_length, tracking);
	if(ret==DPI_PROTOCOL_NO_MATCHES){
		debug_print("%s\n", "[ssl.c] An error occurred in the SSL protocol manager.");
		return DPI_PROTOCOL_ERROR;
	}else{
		debug_print("%s\n", "[ssl.c] SSL callback manager exits.");
		return DPI_PROTOCOL_MATCHES;
	}
}


u_int8_t check_ssl(dpi_library_state_t* state, dpi_pkt_infos_t* pkt, const unsigned char* payload, u_int32_t data_length, dpi_tracking_informations_t* t)
{
	int res;
	debug_print("Checking ssl with size %d, direction %d\n", data_length, pkt->direction);
	if(state->ssl_callbacks != NULL)
	{
		t->ssl_information[pkt->direction].callbacks = state->ssl_callbacks;
		t->ssl_information[pkt->direction].callbacks_user_data = state->ssl_callbacks_user_data;
	}
	if(t->ssl_information[pkt->direction].ssl_detected == 1)
	{
		debug_print("%s\n", "SSL already detected, not needed additional checks");
		return DPI_PROTOCOL_MATCHES;
	}
	if(t->ssl_information[pkt->direction].pkt_buffer == NULL)
	{
		res = detectSSLFromCertificate((uint8_t *)payload, data_length, &t->ssl_information[pkt->direction], pkt);
		debug_print("Result %d\n", res);
		if(res > 0)
		{
			if(res == 3)
			{
				t->ssl_information[pkt->direction].pkt_buffer = (uint8_t *)malloc(data_length);
				memcpy(t->ssl_information[pkt->direction].pkt_buffer, payload, data_length);
				t->ssl_information[pkt->direction].pkt_size = data_length;
				return DPI_PROTOCOL_MORE_DATA_NEEDED;
			}
			t->ssl_information[pkt->direction].ssl_detected = 1;
			return DPI_PROTOCOL_MATCHES;
		}
	} else {
		t->ssl_information[pkt->direction].pkt_buffer = (uint8_t *)realloc(t->ssl_information[pkt->direction].pkt_buffer, t->ssl_information[pkt->direction].pkt_size+data_length);
		memcpy(t->ssl_information[pkt->direction].pkt_buffer+t->ssl_information[pkt->direction].pkt_size, payload, data_length);
		t->ssl_information[pkt->direction].pkt_size += data_length;
		res = detectSSLFromCertificate(t->ssl_information[pkt->direction].pkt_buffer, t->ssl_information[pkt->direction].pkt_size, &t->ssl_information[pkt->direction], pkt);
		debug_print("Checked %d bytes and result %d\n", t->ssl_information[pkt->direction].pkt_size, res);
		if(res > 0)
		{
			if(res == 3)
			{
				return DPI_PROTOCOL_MORE_DATA_NEEDED;
			}
			t->ssl_information[pkt->direction].ssl_detected = 1;
			return DPI_PROTOCOL_MATCHES;
		}
	}
	return DPI_PROTOCOL_NO_MATCHES;
}
