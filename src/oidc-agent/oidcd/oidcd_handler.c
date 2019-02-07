#include "oidcd_handler.h"

#include "defines/agent_values.h"
#include "defines/ipc_values.h"
#include "defines/oidc_values.h"
#include "ipc/pipe.h"
#include "list/list.h"
#include "oidc-agent/agent_state.h"
#include "oidc-agent/httpserver/startHttpserver.h"
#include "oidc-agent/httpserver/termHttpserver.h"
#include "oidc-agent/oidc/device_code.h"
#include "oidc-agent/oidc/flows/access_token_handler.h"
#include "oidc-agent/oidc/flows/code.h"
#include "oidc-agent/oidc/flows/device.h"
#include "oidc-agent/oidc/flows/openid_config.h"
#include "oidc-agent/oidc/flows/registration.h"
#include "oidc-agent/oidc/flows/revoke.h"
#include "oidc-agent/oidcd/parse_internal.h"
#include "utils/crypt/crypt.h"
#include "utils/crypt/cryptUtils.h"
#include "utils/json.h"
#include "utils/listUtils.h"

#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>

void initAuthCodeFlow(const struct oidc_account* account, struct ipcPipe pipes,
                      const char* info) {
  size_t state_len = 24;
  char   state[state_len + 1];
  randomFillBase64UrlSafe(state, state_len);
  state[state_len] = '\0';
  char code_verifier[CODE_VERIFIER_LEN + 1];
  randomFillBase64UrlSafe(code_verifier, CODE_VERIFIER_LEN);
  code_verifier[CODE_VERIFIER_LEN] = '\0';

  char* uri = buildCodeFlowUri(account, state, code_verifier);
  moresecure_memzero(code_verifier, CODE_VERIFIER_LEN);
  if (uri == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
  } else {
    if (info) {
      ipc_writeToPipe(pipes, RESPONSE_STATUS_CODEURI_INFO, STATUS_ACCEPTED, uri,
                      state, info);
    } else {
      ipc_writeToPipe(pipes, RESPONSE_STATUS_CODEURI, STATUS_ACCEPTED, uri,
                      state);
    }
  }
  secFree(uri);
}

void oidcd_handleGen(struct ipcPipe pipes, list_t* loaded_accounts,
                     const char* account_json, const char* flow) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Gen request");
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (!strValid(account_getTokenEndpoint(account))) {
    ipc_writeOidcErrnoToPipe(pipes);
    secFreeAccount(account);
    return;
  }

  int              success = 0;
  list_t*          flows   = parseFlow(flow);
  list_node_t*     current_flow;
  list_iterator_t* it = list_iterator_new(flows, LIST_HEAD);
  while ((current_flow = list_iterator_next(it))) {
    if (strcaseequal(current_flow->val, FLOW_VALUE_REFRESH)) {
      if (getAccessTokenUsingRefreshFlow(account, FORCE_NEW_TOKEN, NULL,
                                         pipes) != NULL) {
        success = 1;
        break;
      } else if (flows->len == 1) {
        ipc_writeOidcErrnoToPipe(pipes);
        list_iterator_destroy(it);
        list_destroy(flows);
        secFreeAccount(account);
        return;
      }
    } else if (strcaseequal(current_flow->val, FLOW_VALUE_PASSWORD)) {
      if (getAccessTokenUsingPasswordFlow(account, pipes) == OIDC_SUCCESS) {
        success = 1;
        break;
      } else if (flows->len == 1) {
        ipc_writeOidcErrnoToPipe(pipes);
        list_iterator_destroy(it);
        list_destroy(flows);
        secFreeAccount(account);
        return;
      }
    } else if (strcaseequal(current_flow->val, FLOW_VALUE_CODE) &&
               hasRedirectUris(account)) {
      initAuthCodeFlow(account, pipes, NULL);
      list_iterator_destroy(it);
      list_destroy(flows);
      secFreeAccount(account);
      return;
    } else if (strcaseequal(current_flow->val, FLOW_VALUE_DEVICE)) {
      struct oidc_device_code* dc = initDeviceFlow(account);
      if (dc == NULL) {
        ipc_writeOidcErrnoToPipe(pipes);
        list_iterator_destroy(it);
        list_destroy(flows);
        secFreeAccount(account);
        return;
      }
      char* json = deviceCodeToJSON(*dc);
      ipc_writeToPipe(pipes, RESPONSE_ACCEPTED_DEVICE, json, account_json);
      secFree(json);
      secFreeDeviceCode(dc);
      list_iterator_destroy(it);
      list_destroy(flows);
      secFreeAccount(account);
      return;
    } else {  // UNKNOWN FLOW
      char* msg;
      if (strcaseequal(current_flow->val, FLOW_VALUE_CODE) &&
          !hasRedirectUris(account)) {
        msg = oidc_sprintf("Only '%s' flow specified, but no redirect uris",
                           FLOW_VALUE_CODE);
      } else {
        msg = oidc_sprintf("Unknown flow '%s'", (char*)current_flow->val);
      }
      ipc_writeToPipe(pipes, RESPONSE_ERROR, msg);
      secFree(msg);
      list_iterator_destroy(it);
      list_destroy(flows);
      secFreeAccount(account);
      return;
    }
  }

  list_iterator_destroy(it);
  list_destroy(flows);

  account_setUsername(account, NULL);
  account_setPassword(account, NULL);
  if (account_refreshTokenIsValid(account) && success) {
    char* json = accountToJSONString(account);
    ipc_writeToPipe(pipes, RESPONSE_STATUS_CONFIG, STATUS_SUCCESS, json);
    secFree(json);
    addAccountToList(loaded_accounts, account);
  } else {
    ipc_writeToPipe(pipes, RESPONSE_ERROR,
                    success ? "OIDP response does not contain a refresh token"
                            : "No flow was successfull.");
    secFreeAccount(account);
  }
}

/**
 * checks if an account is feasable (issuer config / AT retrievable) and adds it
 * to the loaded list; does not check if account already loaded.
 */
oidc_error_t addAccount(struct ipcPipe pipes, struct oidc_account* account,
                        list_t* loaded_accounts) {
  if (account == NULL || loaded_accounts == NULL) {
    oidc_setArgNullFuncError(__func__);
    return oidc_errno;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    return oidc_errno;
  }
  if (!strValid(account_getTokenEndpoint(account))) {
    return oidc_errno;
  }
  if (getAccessTokenUsingRefreshFlow(account, FORCE_NEW_TOKEN, NULL, pipes) ==
      NULL) {
    return oidc_errno;
  }
  addAccountToList(loaded_accounts, account);
  return OIDC_SUCCESS;
}

void oidcd_handleAdd(struct ipcPipe pipes, list_t* loaded_accounts,
                     const char* account_json, const char* timeout_str,
                     const char* confirm_str) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Add request");
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  time_t timeout =
      strValid(timeout_str) ? atol(timeout_str) : agent_state.defaultTimeout;
  account_setDeath(account, timeout ? time(NULL) + timeout : 0);
  if (strToInt(confirm_str)) {
    account_setConfirmationRequired(account);
  }
  struct oidc_account* found = NULL;
  if ((found = getAccountFromList(loaded_accounts, account)) != NULL) {
    if (account_getDeath(found) != account_getDeath(account)) {
      account_setDeath(found, account_getDeath(account));
      char* msg = oidc_sprintf(
          "account already loaded. Lifetime set to %lu seconds.", timeout ?: 0);
      ipc_writeToPipe(pipes, RESPONSE_SUCCESS_INFO, msg);
      secFree(msg);
    } else {
      ipc_writeToPipe(pipes, RESPONSE_SUCCESS_INFO, "account already loaded.");
    }
    addAccountToList(loaded_accounts, found);  // reencrypting sensitive data
    secFreeAccount(account);
    return;
  }
  if (addAccount(pipes, account, loaded_accounts) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Loaded Account. Used timeout of %lu",
         timeout);
  if (timeout > 0) {
    char* msg = oidc_sprintf("Lifetime set to %lu seconds", timeout);
    ipc_writeToPipe(pipes, RESPONSE_SUCCESS_INFO, msg);
    secFree(msg);
  } else {
    ipc_writeToPipe(pipes, RESPONSE_STATUS_SUCCESS);
  }
}

void oidcd_handleDelete(struct ipcPipe pipes, list_t* loaded_accounts,
                        const char* account_json) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Delete request");
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  list_node_t* found_node = NULL;
  if ((found_node = findInList(loaded_accounts, account)) == NULL) {
    secFreeAccount(account);
    ipc_writeToPipe(pipes, RESPONSE_ERROR,
                    "Could not revoke token: account not loaded");
    return;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (revokeToken(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    char* error = oidc_sprintf("Could not revoke token: %s", oidc_serror());
    ipc_writeToPipe(pipes, RESPONSE_ERROR, error);
    secFree(error);
    return;
  }
  list_remove(loaded_accounts, found_node);
  secFreeAccount(account);
  ipc_writeToPipe(pipes, RESPONSE_STATUS_SUCCESS);
}

void oidcd_handleRm(struct ipcPipe pipes, list_t* loaded_accounts,
                    char* account_name) {
  if (account_name == NULL) {
    ipc_writeToPipe(
        pipes, RESPONSE_BADREQUEST,
        "Have to provide shortname of the account config that should be "
        "removed.");
    return;
  }
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Remove request for config '%s'",
         account_name);
  struct oidc_account key   = {.shortname = account_name};
  list_node_t*        found = NULL;
  if ((found = findInList(loaded_accounts, &key)) == NULL) {
    ipc_writeToPipe(pipes, RESPONSE_ERROR, ACCOUNT_NOT_LOADED);
    return;
  }
  list_remove(loaded_accounts, found);
  ipc_writeToPipe(pipes, RESPONSE_STATUS_SUCCESS);
}

void oidcd_handleRemoveAll(struct ipcPipe pipes, list_t** loaded_accounts) {
  list_t* empty = list_new();
  empty->free   = (*loaded_accounts)->free;
  empty->match  = (*loaded_accounts)->match;
  list_destroy(*loaded_accounts);
  *loaded_accounts = empty;
  ipc_writeToPipe(pipes, RESPONSE_STATUS_SUCCESS);
}

oidc_error_t oidcd_autoload(struct ipcPipe pipes, list_t* loaded_accounts,
                            char* short_name, const char* application_hint) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Send autoload request for '%s'",
         short_name);
  char* res = ipc_communicateThroughPipe(pipes, INT_REQUEST_AUTOLOAD,
                                         short_name, application_hint ?: "");
  if (res == NULL) {
    return oidc_errno;
  }
  char* config = parseForConfig(res);
  if (config == NULL) {
    return oidc_errno;
  }
  struct oidc_account* account = getAccountFromJSON(config);
  account_setDeath(account, agent_state.defaultTimeout
                                ? time(NULL) + agent_state.defaultTimeout
                                : 0);
  if (addAccount(pipes, account, loaded_accounts) != OIDC_SUCCESS) {
    secFreeAccount(account);
    return oidc_errno;
  }
  return OIDC_SUCCESS;
}

oidc_error_t oidcd_getConfirmation(struct ipcPipe pipes, char* short_name,
                                   const char* application_hint) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Send confirm request for '%s'", short_name);
  char* res = ipc_communicateThroughPipe(pipes, INT_REQUEST_CONFIRM, short_name,
                                         application_hint ?: "");
  if (res == NULL) {
    return oidc_errno;
  }
  oidc_errno = parseForErrorCode(res);
  return oidc_errno;
}

void oidcd_handleToken(struct ipcPipe pipes, list_t* loaded_accounts,
                       char* short_name, const char* min_valid_period_str,
                       const char* scope, const char* application_hint,
                       const struct arguments* arguments) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Token request from %s",
         application_hint);
  if (short_name == NULL) {
    ipc_writeToPipe(pipes, RESPONSE_ERROR,
                    "Bad request. Required field '" IPC_KEY_SHORTNAME
                    "' not present.");
    return;
  }
  struct oidc_account key = {.shortname = short_name};
  time_t              min_valid_period =
      min_valid_period_str != NULL ? strToInt(min_valid_period_str) : 0;
  struct oidc_account* account = getAccountFromList(loaded_accounts, &key);
  if (account == NULL) {
    if (arguments->no_autoload) {
      ipc_writeToPipe(pipes, RESPONSE_ERROR, ACCOUNT_NOT_LOADED);
      return;
    }
    oidc_error_t autoload_error =
        oidcd_autoload(pipes, loaded_accounts, short_name, application_hint);
    switch (autoload_error) {
      case OIDC_SUCCESS:
        account = getAccountFromList(loaded_accounts, &key);
        break;
      case OIDC_EUSRPWCNCL:
        ipc_writeToPipe(pipes, RESPONSE_ERROR, ACCOUNT_NOT_LOADED);
        return;
      default: ipc_writeOidcErrnoToPipe(pipes); return;
    }
  } else if (arguments->confirm || account_getConfirmationRequired(account)) {
    if (oidcd_getConfirmation(pipes, short_name, application_hint) !=
        OIDC_SUCCESS) {
      ipc_writeOidcErrnoToPipe(pipes);
      return;
    }
  }
  char* access_token =
      getAccessTokenUsingRefreshFlow(account, min_valid_period, scope, pipes);
  addAccountToList(loaded_accounts, account);  // reencrypting
  if (access_token == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  ipc_writeToPipe(pipes, RESPONSE_STATUS_ACCESS, STATUS_SUCCESS, access_token,
                  account_getIssuerUrl(account),
                  account_getTokenExpiresAt(account));
  if (strValid(scope)) {
    secFree(access_token);
  }
}

/**
 * Removed in version 2.0.0
 */
// void oidcd_handleList(int sock, list_t* loaded_accounts) {
//   syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle list request");
//   char* accountList = getAccountNameList(loaded_accounts);
//   server_ipc_write(sock, RESPONSE_STATUS_ACCOUNT, STATUS_SUCCESS,
//             oidc_errno == OIDC_EARGNULL ? "[]" : accountList);
//   secFree(accountList);
// }

void oidcd_handleRegister(struct ipcPipe pipes, list_t* loaded_accounts,
                          const char* account_json, const char* flows_json_str,
                          const char* access_token) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle Register request for flows: '%s'",
         flows_json_str);
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "daeSetByUser is: %d",
         issuer_getDeviceAuthorizationEndpointIsSetByUser(
             account_getIssuer(account)));
  if (NULL != findInList(loaded_accounts, account)) {
    secFreeAccount(account);
    ipc_writeToPipe(
        pipes, RESPONSE_ERROR,
        "An account with this shortname is already loaded. I will not "
        "register a new one.");
    return;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "daeSetByUser is: %d",
         issuer_getDeviceAuthorizationEndpointIsSetByUser(
             account_getIssuer(account)));
  list_t* flows = JSONArrayStringToList(flows_json_str);
  if (flows == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  char* res = dynamicRegistration(account, flows, access_token);
  if (res == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
  } else {
    if (!isJSONObject(res)) {
      char* escaped = escapeCharInStr(res, '"');
      ipc_writeToPipe(pipes, RESPONSE_ERROR_INFO,
                      "Received no JSON formatted response.", escaped);
      secFree(escaped);
    } else {
      cJSON* json_res1 = stringToJson(res);
      if (jsonHasKey(json_res1, OIDC_KEY_ERROR)) {  // first failed
        list_removeIfFound(flows, list_find(flows, FLOW_VALUE_PASSWORD));
        char* res2 = dynamicRegistration(
            account, flows, access_token);  // TODO only try this if password
                                            // flow was in flow list
        if (res2 == NULL) {                 // second failed complety
          ipc_writeOidcErrnoToPipe(pipes);
        } else {
          if (jsonStringHasKey(res2,
                               OIDC_KEY_ERROR)) {  // first and second failed
            char* error = getJSONValue(json_res1, OIDC_KEY_ERROR_DESCRIPTION);
            if (error == NULL) {
              error = getJSONValue(json_res1, OIDC_KEY_ERROR);
            }
            ipc_writeToPipe(pipes, RESPONSE_ERROR, error);
            secFree(error);
          } else {  // first failed, second successful
            ipc_writeToPipe(pipes, RESPONSE_SUCCESS_CLIENT, res2);
          }
        }
        secFree(res2);
      } else {  // first was successfull
        char* scopes = getJSONValueFromString(res, OIDC_KEY_SCOPE);
        if (!strSubStringCase(scopes, OIDC_SCOPE_OPENID) ||
            !strSubStringCase(scopes, OIDC_SCOPE_OFFLINE_ACCESS)) {
          // did not get all scopes necessary for oidc-agent
          oidc_errno = OIDC_EUNSCOPE;
          ipc_writeToPipe(pipes, RESPONSE_ERROR_CLIENT, oidc_serror(), res);
        }
        secFree(scopes);
        ipc_writeToPipe(pipes, RESPONSE_SUCCESS_CLIENT, res);
      }
      secFreeJson(json_res1);
    }
  }
  list_destroy(flows);
  secFree(res);
  secFreeAccount(account);
}

void oidcd_handleCodeExchange(struct ipcPipe pipes, list_t* loaded_accounts,
                              const char* account_json, const char* code,
                              const char* redirect_uri, const char* state,
                              char* code_verifier) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle codeExchange request");
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (getAccessTokenUsingAuthCodeFlow(account, code, redirect_uri,
                                      code_verifier, pipes) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  if (account_refreshTokenIsValid(account)) {
    char* json = accountToJSONString(account);
    ipc_writeToPipe(pipes, RESPONSE_STATUS_CONFIG, STATUS_SUCCESS, json);
    secFree(json);
    account_setUsedState(account, oidc_sprintf("%s", state));
    addAccountToList(loaded_accounts, account);
  } else {
    ipc_writeToPipe(pipes, RESPONSE_ERROR, "Could not get a refresh token");
    secFreeAccount(account);
  }
}

void oidcd_handleDeviceLookup(struct ipcPipe pipes, list_t* loaded_accounts,
                              const char* account_json,
                              const char* device_json) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle deviceLookup request");
  struct oidc_account* account = getAccountFromJSON(account_json);
  if (account == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  struct oidc_device_code* dc = getDeviceCodeFromJSON(device_json);
  if (dc == NULL) {
    ipc_writeOidcErrnoToPipe(pipes);
    secFreeAccount(account);
    return;
  }
  if (getIssuerConfig(account) != OIDC_SUCCESS) {
    secFreeAccount(account);
    ipc_writeOidcErrnoToPipe(pipes);
    secFreeDeviceCode(dc);
    return;
  }
  if (getAccessTokenUsingDeviceFlow(account, oidc_device_getDeviceCode(*dc),
                                    pipes) != OIDC_SUCCESS) {
    secFreeAccount(account);
    secFreeDeviceCode(dc);
    ipc_writeOidcErrnoToPipe(pipes);
    return;
  }
  secFreeDeviceCode(dc);
  if (account_refreshTokenIsValid(account)) {
    char* json = accountToJSONString(account);
    ipc_writeToPipe(pipes, RESPONSE_STATUS_CONFIG, STATUS_SUCCESS, json);
    secFree(json);
    addAccountToList(loaded_accounts, account);
  } else {
    ipc_writeToPipe(pipes, RESPONSE_ERROR, "Could not get a refresh token");
    secFreeAccount(account);
  }
}

void oidcd_handleStateLookUp(struct ipcPipe pipes, list_t* loaded_accounts,
                             char* state) {
  syslog(LOG_AUTHPRIV | LOG_DEBUG, "Handle codeLookUp request");
  struct oidc_account key      = {.usedState = state};
  void*               oldMatch = loaded_accounts->match;
  loaded_accounts->match       = (int (*)(void*, void*)) & account_matchByState;
  struct oidc_account* account = getAccountFromList(loaded_accounts, &key);
  loaded_accounts->match       = oldMatch;
  if (account == NULL) {
    char* info =
        oidc_sprintf("No loaded account info found for state=%s", state);
    ipc_writeToPipe(pipes, RESPONSE_STATUS_INFO, STATUS_NOTFOUND, info);
    secFree(info);
    return;
  }
  account_setUsedState(account, NULL);
  char* config = accountToJSONString(account);
  ipc_writeToPipe(pipes, RESPONSE_STATUS_CONFIG, STATUS_SUCCESS, config);
  secFree(config);
  addAccountToList(loaded_accounts, account);  // reencrypting
  termHttpServer(state);
}

void oidcd_handleTermHttp(struct ipcPipe pipes, const char* state) {
  termHttpServer(state);
  ipc_writeToPipe(pipes, RESPONSE_SUCCESS);
}

void oidcd_handleLock(struct ipcPipe pipes, const char* password,
                      list_t* loaded_accounts, int _lock) {
  if (_lock) {
    if (lock(loaded_accounts, password) == OIDC_SUCCESS) {
      ipc_writeToPipe(pipes, RESPONSE_SUCCESS_INFO, "Agent locked");
      return;
    }
  } else {
    if (unlock(loaded_accounts, password) == OIDC_SUCCESS) {
      ipc_writeToPipe(pipes, RESPONSE_SUCCESS_INFO, "Agent unlocked");
      return;
    }
  }
  ipc_writeOidcErrnoToPipe(pipes);
}
