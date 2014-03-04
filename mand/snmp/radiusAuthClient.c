/*
 * Note: this file originally auto-generated by mib2c using
 *        : mib2c.scalar.conf,v 1.9 2005/01/07 09:37:18 dts12 Exp $
 */


#include <stdio.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/time.h>

#include "dm_token.h"
#include "dm_store.h"
#include "dm_index.h"

#define SDEBUG
#include "dm_assert.h"
#include "debug.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/library/snmp_assert.h>

#include "snmp_helper.h"
#include "radiusAuthClient.h"

static oid radiusAuthClient_oid[] = { 1, 3, 6, 1, 2, 1, 67, 1, 2, 1, 1 };

#define radiusAuthClientInvalidServerAddresses_oid	1
#define radiusAuthClientIdentifier_oid			2
#define radiusAuthServerTable_oid			3
#define radiusAuthServerExtTable_oid			4

static int
radiusAuthClient_handler(netsnmp_mib_handler *handler,
			 netsnmp_handler_registration *reginfo,
			 netsnmp_agent_request_info *reqinfo,
			 netsnmp_request_info *requests);

/** Initializes the radiusAuthClient module */
void
init_radiusAuthClient(void)
{
	netsnmp_handler_registration *reginfo;

	/*
	 * register ourselves with the agent as a group of scalars...
	 */
 
	DEBUGMSGTL(("radiusAuthClient", "Initializing\n"));

	reginfo = netsnmp_create_handler_registration("radiusAuthClient",
						      radiusAuthClient_handler,
						      radiusAuthClient_oid, OID_LENGTH(radiusAuthClient_oid),
						      HANDLER_CAN_RONLY);
	netsnmp_register_scalar_group(reginfo, radiusAuthClientInvalidServerAddresses_oid, radiusAuthServerExtTable_oid);
}

int
radiusAuthClient_handler(netsnmp_mib_handler *handler,
		       netsnmp_handler_registration *reginfo,
		       netsnmp_agent_request_info *reqinfo,
		       netsnmp_request_info *requests)
{
	netsnmp_request_info  *request;
	netsnmp_variable_list *requestvb;
	unsigned int ret_value = 0;
	oid      subid;
	int      type;

	ENTER();

	/*
	 * 
	 *
	 */
	DEBUGMSGTL(("radiusAuthClient", "Handler - mode %s\n", se_find_label_in_slist("agent_mode", reqinfo->mode)));
	switch (reqinfo->mode) {
	case MODE_GET:
		for (request = requests; request; request = request->next) {
			requestvb = request->requestvb;
			subid = requestvb->name[OID_LENGTH(radiusAuthClient_oid)];  /* XXX */

			type = ASN_COUNTER;

			DEBUGMSGTL(( "radiusAuthClient", "oid: "));
			DEBUGMSGOID(("radiusAuthClient", requestvb->name, requestvb->name_length));
			DEBUGMSG((   "radiusAuthClient", "\n"));
			switch (subid) {
			case radiusAuthClientInvalidServerAddresses_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Authentication.Stats */
				ret_value = dm_get_uint_by_selector((dm_selector){ dm__InternetGatewayDevice,
							dm__IGD_X_TPLINO_NET_SessionControl,
							dm__IGD_SCG_RadiusClient,
							dm__IGD_SCG_RC_Authentication,
							dm__IGD_SCG_RC_Auth_Stats,
							dm__IGD_SCG_RC_Auth_Stats_InvalidServerAddresses, 0});
				break;

			case radiusAuthClientIdentifier_oid:
				snmp_set_var_typed_value(requests->requestvb, ASN_OCTET_STR, (unsigned char *)"", 0);
				continue;

			case radiusAuthServerTable_oid:
			case radiusAuthServerExtTable_oid:
				/*
				 * These are not actually valid scalar objects.
				 * The table registration should take precedence,
				 *   so skip thess subtree, regardless of architecture.
				 */
				netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
				continue;
			}
			snmp_set_var_typed_value(request->requestvb, type, (u_char *)&ret_value, sizeof(ret_value));
		}
		break;

	case MODE_GETNEXT:
	case MODE_GETBULK:
	case MODE_SET_RESERVE1:
	case MODE_SET_RESERVE2:
	case MODE_SET_ACTION:
	case MODE_SET_COMMIT:
	case MODE_SET_FREE:
	case MODE_SET_UNDO:
		snmp_log(LOG_WARNING, "radiusAuthClient: Unsupported mode (%d)\n", reqinfo->mode);
		break;
	default:
		snmp_log(LOG_WARNING, "radiusAuthClient: Unrecognised mode (%d)\n", reqinfo->mode);
		break;
	}
	
	return SNMP_ERR_NOERROR;
}
