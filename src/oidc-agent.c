#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <time.h>
#include <argp.h>
#include <ctype.h>
#include <libgen.h>

#include "oidc-agent.h"
#include "oidc.h"
#include "ipc.h"
#include "provider.h"
#include "oidc_utilities.h"
#include "oidc_error.h"
#include "version.h"

const char *argp_program_version = AGENT_VERSION;

const char *argp_program_bug_address = BUG_ADDRESS;

/* This structure is used by main to communicate with parse_opt. */
struct arguments {
  int kill_flag;
  int debug;
  int console;
};

/*
   OPTIONS.  Field 1 in ARGP.
   Order of fields: {NAME, KEY, ARG, FLAGS, DOC}.
   */
static struct argp_option options[] = {
  {"kill", 'k', 0, 0, "Kill the current agent (given by the OIDCD_PID environment variable).", 0},
  {"debug", 'g', 0, 0, "sets the log level to DEBUG", 0},
  {"console", 'c', 0, 0, "runs oidc-agent on the console, without daemonizing", 0},
  {0}
};

/*
   PARSER. Field 2 in ARGP.
   Order of parameters: KEY, ARG, STATE.
   */
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
  struct arguments *arguments = state->input;

  switch (key)
  {
    case 'k':
      arguments->kill_flag = 1;
      break;
    case 'g':
      arguments->debug = 1;
      break;
    case 'c':
      arguments->console = 1;
      break;
    case ARGP_KEY_ARG:
      argp_usage(state);
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

/*
   ARGS_DOC. Field 3 in ARGP.
   A description of the non-option command-line arguments
   that we accept.
   */
static char args_doc[] = "";

/*
   DOC.  Field 4 in ARGP.
   Program documentation.
   */
static char doc[] = "oidc-agent -- A agent to manage oidc token";

/*
   The ARGP structure itself.
   */
static struct argp argp = {options, parse_opt, args_doc, doc};


void sig_handler(int signo) {
  switch(signo) {
    case SIGSEGV:
      syslog(LOG_AUTHPRIV|LOG_EMERG, "Caught Signal SIGSEGV");
      break;
    default: 
      syslog(LOG_AUTHPRIV|LOG_EMERG, "Caught Signal %d", signo);
  }
  exit(signo);
}

void daemonize() {
  pid_t pid;
  if((pid = fork ()) ==-1) {
    syslog(LOG_AUTHPRIV|LOG_ALERT, "fork %m");
    exit(EXIT_FAILURE);
  } else if(pid > 0) {
    exit (EXIT_SUCCESS);
  }
  if(setsid() < 0) {
    exit (EXIT_FAILURE);
  }
  signal(SIGHUP, SIG_IGN);
  if((pid = fork ()) ==-1) {
    syslog(LOG_AUTHPRIV|LOG_ALERT, "fork %m");
    exit(EXIT_FAILURE);
  } else if(pid > 0) {
    printf("%s=%d; export %s;\n", OIDC_PID_ENV_NAME, pid, OIDC_PID_ENV_NAME);
    printf("echo Agent pid $%s\n", OIDC_PID_ENV_NAME);
    exit (EXIT_SUCCESS);
  }
  chdir ("/");
  umask (0);
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  open("/dev/null", O_RDONLY);
  open("/dev/null", O_RDWR);
  open("/dev/null", O_RDWR);
}

void handleGen(int sock, struct oidc_provider** loaded_p, size_t* loaded_p_count, char* provider_json) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle Gen request");
  struct oidc_provider* provider = getProviderFromJSON(provider_json);
  if(provider==NULL) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  getEndpoints(provider);
  if(!isValid(provider_getTokenEndpoint(*provider))) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  if(retrieveAccessToken(provider, FORCE_NEW_TOKEN)!=OIDC_SUCCESS) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror()); 
    freeProvider(provider);
    return;
  } 
  if(isValid(provider_getRefreshToken(*provider))) {
    ipc_write(sock, RESPONSE_STATUS_ENDPOINT_REFRESH, "success", provider_getTokenEndpoint(*provider), provider_getAuthorizationEndpoint(*provider), provider_getRegistrationEndpoint(*provider), provider_getRevocationEndpoint(*provider), provider_getRefreshToken(*provider));
  } else {
    ipc_write(sock, RESPONSE_STATUS_ENDPOINT, "success", provider_getTokenEndpoint(*provider), provider_getAuthorizationEndpoint(*provider), provider_getRegistrationEndpoint(*provider), provider_getRevocationEndpoint(*provider));   }
  provider_setUsername(provider, NULL);
  provider_setPassword(provider, NULL);
  *loaded_p = removeProvider(*loaded_p, loaded_p_count, *provider);
  *loaded_p = addProvider(*loaded_p, loaded_p_count, *provider);
  clearFree(provider, sizeof(*provider));
} 

void handleAdd(int sock, struct oidc_provider** loaded_p, size_t* loaded_p_count, char* provider_json) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle Add request");
  struct oidc_provider* provider = getProviderFromJSON(provider_json);
  if(provider==NULL) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  if(NULL!=findProvider(*loaded_p, *loaded_p_count, *provider)) {
    freeProvider(provider);
    ipc_write(sock, RESPONSE_ERROR, "provider already loaded");
    return;
  }
  if(retrieveAccessTokenRefreshFlowOnly(provider, FORCE_NEW_TOKEN)!=OIDC_SUCCESS) {
    char* oldTokenEndpoint = calloc(sizeof(char), strlen(provider_getTokenEndpoint(*provider))+1);
    strcpy(oldTokenEndpoint, provider_getTokenEndpoint(*provider));
    oidc_error_t error = oidc_errno;
    getEndpoints(provider);
    if(oldTokenEndpoint && provider_getTokenEndpoint(*provider) && strcmp(oldTokenEndpoint, provider_getTokenEndpoint(*provider))!=0) {
      clearFreeString(oldTokenEndpoint);
      if(retrieveAccessTokenRefreshFlowOnly(provider, FORCE_NEW_TOKEN)!=OIDC_SUCCESS) {
        freeProvider(provider);
        ipc_write(sock, RESPONSE_ERROR, oidc_perror());
        return;
      }
    } else {
      clearFreeString(oldTokenEndpoint);
      freeProvider(provider);
      oidc_errno = error;
      ipc_write(sock, RESPONSE_ERROR, oidc_perror()); 
      return;
    } 
  }
  *loaded_p = addProvider(*loaded_p, loaded_p_count, *provider);
  clearFree(provider, sizeof(*provider));
  ipc_write(sock, RESPONSE_STATUS_SUCCESS);
}

void handleRm(int sock, struct oidc_provider** loaded_p, size_t* loaded_p_count, char* provider_json, int revoke) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle Remove request");
  struct oidc_provider* provider = getProviderFromJSON(provider_json);
  if(provider==NULL) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  if(NULL==findProvider(*loaded_p, *loaded_p_count, *provider)) {
    freeProvider(provider);
    ipc_write(sock, RESPONSE_ERROR, revoke ? "Could not revoke token: provider not loaded" : "provider not loaded");
    return;
  }
  if(revoke && revokeToken(provider)!=OIDC_SUCCESS) {
    freeProvider(provider);
    ipc_write(sock, RESPONSE_ERROR, "Could not revoke token: %s", oidc_perror());
    return;
  }
  *loaded_p = removeProvider(*loaded_p, loaded_p_count, *provider);
  freeProvider(provider);
  ipc_write(sock, RESPONSE_STATUS_SUCCESS);
}

void handleToken(int sock, struct oidc_provider* loaded_p, size_t loaded_p_count, char* short_name, char* min_valid_period_str) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle Token request");
  if(short_name==NULL || min_valid_period_str== NULL) {
    ipc_write(sock, RESPONSE_ERROR, "Bad request. Need provider name and min_valid_period for getting access token.");
    return;
  }
  struct oidc_provider key = {0, short_name, 0};
  time_t min_valid_period = atoi(min_valid_period_str);
  struct oidc_provider* provider = findProvider(loaded_p, loaded_p_count, key);
  if(provider==NULL) {
    ipc_write(sock, RESPONSE_ERROR, "Provider not loaded.");
    return;
  }
  if(retrieveAccessTokenRefreshFlowOnly(provider, min_valid_period)!=0) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  ipc_write(sock, RESPONSE_STATUS_ACCESS, "success", provider_getAccessToken(*provider));
}

void handleList(int sock, struct oidc_provider* loaded_p, size_t loaded_p_count) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle list request");
  char* providerList = getProviderNameList(loaded_p, loaded_p_count);
  ipc_write(sock, RESPONSE_STATUS_PROVIDER, "success", oidc_errno==OIDC_EARGNULL ? "[]" : providerList);
  clearFreeString(providerList);
}

void handleRegister(int sock, struct oidc_provider* loaded_p, size_t loaded_p_count, char* provider_json) {
  syslog(LOG_AUTHPRIV|LOG_DEBUG, "Handle Register request");
  struct oidc_provider* provider = getProviderFromJSON(provider_json);
  if(provider==NULL) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  if(NULL!=findProvider(loaded_p, loaded_p_count, *provider)) {
    freeProvider(provider);
    ipc_write(sock, RESPONSE_ERROR, "A provider with this shortname is already loaded. I will not register a new one.");
    return;
  }
  if(getEndpoints(provider)!=OIDC_SUCCESS) {
    freeProvider(provider);
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
    return;
  }
  char* res = dynamicRegistration(provider, 1);
  if(res==NULL) {
    ipc_write(sock, RESPONSE_ERROR, oidc_perror());
  } else {
    if(json_hasKey(res, "error")) { // first failed
      char* res2 = dynamicRegistration(provider, 0);
      if(res2==NULL) { //second failed complety
        ipc_write(sock, RESPONSE_ERROR, oidc_perror());
      } else {
        if(json_hasKey(res2, "error")) { // first and second failed
          ipc_write(sock, RESPONSE_ERROR, res); //TODO sent both responses
        } else { // first failed, seconds successfull, still need the grant_types.
          char* error = getJSONValue(res, "error_description");
          if(error==NULL) {
            error = getJSONValue(res, "error");
          }
          char* fmt = "The client was registered with the resulting config. It is not usable for oidc-agent in that way. Please contact the provider to update the client configuration.\nprovider: %s\nclient_id: %s\nadditional needed grant_types: password";
          char* client_id = getJSONValue(res2, "client_id");
          char* send = calloc(sizeof(char), snprintf(NULL, 0, fmt, provider_getIssuer(*provider), client_id)+1);
          sprintf(send, fmt, provider_getIssuer(*provider), client_id);
          clearFreeString(client_id);
          ipc_write(sock, RESPONSE_ERROR_CLIENT_INFO, error, res2, send);
          clearFreeString(send);
          clearFreeString(error);
        }
      }
      clearFreeString(res2);
    } else { // first was successfull
      ipc_write(sock, RESPONSE_SUCCESS_CLIENT, res);
    }
  }
  clearFreeString(res);
  freeProvider(provider);
}

int main(int argc, char** argv) {
  openlog("oidc-agent", LOG_CONS|LOG_PID, LOG_AUTHPRIV);
  setlogmask(LOG_UPTO(LOG_NOTICE));
  struct arguments arguments;

  /* Set argument defaults */
  arguments.kill_flag = 0;
  arguments.console = 0;
  arguments.debug = 0;

  argp_parse (&argp, argc, argv, 0, 0, &arguments);
  if(arguments.debug) {
    setlogmask(LOG_UPTO(LOG_DEBUG));
  }

  if(arguments.kill_flag) {
    char* pidstr = getenv(OIDC_PID_ENV_NAME);
    if(pidstr == NULL) {
      fprintf(stderr, "%s not set, cannot kill Agent\n", OIDC_PID_ENV_NAME);
      exit(EXIT_FAILURE);
    }
    pid_t pid = atoi(pidstr);
    if(0 == pid) {
      fprintf(stderr, "%s not set to a valid pid: %s\n", OIDC_PID_ENV_NAME, pidstr);
      exit(EXIT_FAILURE);
    }
    if (kill(pid, SIGTERM) == -1) {
      perror("kill");
      exit(EXIT_FAILURE);
    } else {
      unlink(getenv(OIDC_SOCK_ENV_NAME));
      rmdir(dirname(getenv(OIDC_SOCK_ENV_NAME)));
      printf("unset %s;\n", OIDC_SOCK_ENV_NAME);
      printf("unset %s;\n", OIDC_PID_ENV_NAME);
      printf("echo Agent pid %d killed;\n", pid);
      exit(EXIT_SUCCESS);
    }
  }

  // signal(SIGSEGV, sig_handler);

  // TODO we can move some of this stuff behind daemonize, but tmp dir has to be
  // created, env var printed, and socket_path some how saved to use
  struct connection* listencon = calloc(sizeof(struct connection), 1);
  if(ipc_init(listencon, OIDC_SOCK_ENV_NAME, 1)!=OIDC_SUCCESS) {
    fprintf(stderr, "%s\n", oidc_perror());
    exit(EXIT_FAILURE);
  }
  if(!arguments.console) {
    daemonize();
  }

  ipc_bindAndListen(listencon);

  struct oidc_provider* loaded_p = NULL;
  struct oidc_provider** loaded_p_addr = &loaded_p;
  size_t loaded_p_count = 0;

  struct connection* clientcons = NULL;
  struct connection** clientcons_addr = &clientcons;
  size_t number_clients = 0;

  while(1) {
    struct connection* con = ipc_async(*listencon, clientcons_addr, &number_clients);
    if(con==NULL) {
      // should never happen
      syslog(LOG_AUTHPRIV|LOG_ALERT, "Something went wrong");
    } else {
      char* q = ipc_read(*(con->msgsock));
      if(NULL!=q) {
        struct key_value pairs[4];
        pairs[0].key = "request"; pairs[0].value = NULL;
        pairs[1].key = "provider"; pairs[1].value = NULL;
        pairs[2].key = "min_valid_period"; pairs[2].value = NULL;
        pairs[3].key = "config"; pairs[3].value = NULL;
        if(getJSONValues(q, pairs, sizeof(pairs)/sizeof(*pairs))<0) {
          ipc_write(*(con->msgsock), "Bad request: %s", oidc_perror());
        } else {
          if(pairs[0].value) {
            if(strcmp(pairs[0].value, "gen")==0) {
              handleGen(*(con->msgsock), loaded_p_addr, &loaded_p_count, pairs[3].value);
            } else if(strcmp(pairs[0].value, "add")==0) {
              handleAdd(*(con->msgsock), loaded_p_addr, &loaded_p_count, pairs[3].value);
            } else if(strcmp(pairs[0].value, "remove")==0) {
              handleRm(*(con->msgsock), loaded_p_addr, &loaded_p_count, pairs[3].value, 0);
            } else if(strcmp(pairs[0].value, "delete")==0) {
              handleRm(*(con->msgsock), loaded_p_addr, &loaded_p_count, pairs[3].value, 1);
            } else if(strcmp(pairs[0].value, "access_token")==0) {
              handleToken(*(con->msgsock), *loaded_p_addr, loaded_p_count, pairs[1].value, pairs[2].value);
            } else if(strcmp(pairs[0].value, "provider_list")==0) {
              handleList(*(con->msgsock), *loaded_p_addr, loaded_p_count);
            } else if(strcmp(pairs[0].value, "register")==0) {
              handleRegister(*(con->msgsock), *loaded_p_addr, loaded_p_count, pairs[3].value);
            } else {
              ipc_write(*(con->msgsock), "Bad request. Unknown request type.");
            }
          } else {
            ipc_write(*(con->msgsock), "Bad request. No request type.");
          }
        }
        clearFreeString(pairs[0].value);
        clearFreeString(pairs[1].value);
        clearFreeString(pairs[2].value);
        clearFreeString(pairs[3].value);
        clearFreeString(q);
      }
      syslog(LOG_AUTHPRIV|LOG_DEBUG, "Remove con from pool");
      clientcons = removeConnection(*clientcons_addr, &number_clients, con);
      clientcons_addr = &clientcons;
    }
  }
  return EXIT_FAILURE;
}




