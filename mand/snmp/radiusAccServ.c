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
#include "radiusAccServ.h"

#define radiusAccServIdent_oid				1
#define radiusAccServUpTime_oid				2
#define radiusAccServResetTime_oid			3
#define radiusAccServConfigReset_oid			4
#define radiusAccServTotalRequests_oid			5
#define radiusAccServTotalInvalidRequests_oid		6
#define radiusAccServTotalDupRequests_oid		7
#define radiusAccServTotalResponses_oid			8
#define radiusAccServTotalMalformedRequests_oid		9
#define radiusAccServTotalBadAuthenticators_oid		10
#define radiusAccServTotalPacketsDropped_oid		11
#define radiusAccServTotalNoRecords_oid			12
#define radiusAccServTotalUnknownTypes_oid		13
#define radiusAuthClientTable_oid			14
#define radiusAuthClientExtTable_oid			15

static oid radiusAccServ_oid[] = { 1, 3, 6, 1, 2, 1, 67, 2, 1, 1, 1 };

static int
radiusAccServ_handler(netsnmp_mib_handler *handler,
		       netsnmp_handler_registration *reginfo,
		       netsnmp_agent_request_info *reqinfo,
		       netsnmp_request_info *requests);

/** Initializes the radiusAccServ module */
void
init_radiusAccServ(void)
{
	netsnmp_handler_registration *reginfo;

	/*
	 * register ourselves with the agent as a group of scalars...
	 */
 
	DEBUGMSGTL(("radiusAccServ", "Initializing\n"));

	reginfo = netsnmp_create_handler_registration("radiusAccServ",
						      radiusAccServ_handler,
						      radiusAccServ_oid, OID_LENGTH(radiusAccServ_oid),
						      HANDLER_CAN_RONLY);
	netsnmp_register_scalar_group(reginfo, radiusAccServIdent_oid, radiusAuthClientExtTable_oid);
}

int
radiusAccServ_handler(netsnmp_mib_handler *handler,
		       netsnmp_handler_registration *reginfo,
		       netsnmp_agent_request_info *reqinfo,
		       netsnmp_request_info *requests)
{
	struct dm_value_table *globs;
	netsnmp_request_info  *request;
	netsnmp_variable_list *requestvb;
	unsigned int ret_value = 0;
	oid      subid;
	int      type;

	ENTER();

	/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats */
	globs = dm_get_table_by_selector((dm_selector){ dm__InternetGatewayDevice,
				dm__IGD_X_TPLINO_NET_SessionControl,
				dm__IGD_SCG_RadiusServer,
				dm__IGD_SCG_RS_Accounting,
				dm__IGD_SCG_RS_Acct_Stats, 0});
	/*
	 * 
	 *
	 */
	DEBUGMSGTL(("radiusAccServ", "Handler - mode %s\n", se_find_label_in_slist("agent_mode", reqinfo->mode)));
	switch (reqinfo->mode) {
	case MODE_GET:
		for (request = requests; request; request = request->next) {
			requestvb = request->requestvb;
			subid = requestvb->name[OID_LENGTH(radiusAccServ_oid)];  /* XXX */

			if (!globs) {
				netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
				continue;
			}

			type = ASN_COUNTER;

			DEBUGMSGTL(( "radiusAccServ", "oid: "));
			DEBUGMSGOID(("radiusAccServ", requestvb->name, requestvb->name_length));
			DEBUGMSG((   "radiusAccServ", "\n"));
			switch (subid) {
			case radiusAccServIdent_oid:
				snmp_set_var_typed_value(requests->requestvb, ASN_OCTET_STR, (unsigned char *)"", 0);
				continue;

			case radiusAccServUpTime_oid:
			case radiusAccServResetTime_oid:
				/* TODO: fill in sensible values */
				type = ASN_TIMETICKS;
				ret_value = 0;
				break;

			case radiusAccServConfigReset_oid:
				type = ASN_INTEGER;
				ret_value = 4;
				break;

			case radiusAccServTotalRequests_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.Requests */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_Requests);
				break;

			case radiusAccServTotalInvalidRequests_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.InvalidRequests */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_InvalidRequests);
				break;

			case radiusAccServTotalDupRequests_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.DupRequests */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_DupRequests);
				break;
				
			case radiusAccServTotalResponses_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.Responses */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_Responses);
				break;

			case radiusAccServTotalMalformedRequests_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.MalformedRequests */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_MalformedRequests);
				break;

			case radiusAccServTotalBadAuthenticators_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.BadAuthenticators */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_BadAuthenticators);
				break;

			case radiusAccServTotalPacketsDropped_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.PacketsDropped */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_PacketsDropped);
				break;

			case radiusAccServTotalNoRecords_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.NoRecords */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_NoRecords);
				break;

			case radiusAccServTotalUnknownTypes_oid:
				/** VAR: InternetGatewayDevice.X_TPLINO_NET_SessionControl.RadiusServer.Accounting.Stats.UnknownTypes */
				ret_value = dm_get_uint_by_id(globs, dm__IGD_SCG_RS_Acct_Stats_UnknownTypes);
				break;

			case radiusAuthClientTable_oid:
			case radiusAuthClientExtTable_oid:
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
		snmp_log(LOG_WARNING, "mibII/tcp: Unsupported mode (%d)\n", reqinfo->mode);
		break;
	default:
		snmp_log(LOG_WARNING, "mibII/tcp: Unrecognised mode (%d)\n", reqinfo->mode);
		break;
	}
	
	return SNMP_ERR_NOERROR;
}
