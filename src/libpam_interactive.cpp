// =-=-=-=-=-=-=-
// irods includes
#define USE_SSL 1
#include "sslSockComm.h"
#include "rodsDef.h"
#include "msParam.h"
#include "rcConnect.h"
#include "authRequest.h"
#include "authResponse.h"
#include "authCheck.h"
#include "rsAuthRequest.hpp"
#include "rsAuthCheck.hpp"
#include "authPluginRequest.h"
#include "irods_generic_auth_object.hpp"

// =-=-=-=-=-=-=-
#include "irods_auth_plugin.hpp"
#include "irods_auth_constants.hpp"
#include "irods_stacktrace.hpp"
#include "irods_kvp_string_parser.hpp"
#include "irods_client_server_negotiation.hpp"

// =-=-=-=-=-=-=-
// boost includes
#include <boost/lexical_cast.hpp>

// =-=-=-=-=-=-=-
// stl includes
#include <sstream>
#include <string>
#include <iostream>
#include <unistd.h>
#include <algorithm>

// md5
#include <openssl/md5.h>

// =-=-=-=-=-=-=-
// system includes
#include <sys/types.h>
#include <sys/wait.h>

// 3rd party library
#include <json.hpp>


#ifdef RODS_SERVER
#include "pam_handshake/handshake_session.h"
#endif
#include "message.h"
#include "conversation.h"
#include "../whereami/whereami.hpp"

int get64RandomBytes( char *buf );

const char AUTH_PAM_INTERACTIVE_SCHEME[] = "pam_interactive";
const int VERBOSE_LEVEL = 0;

#ifdef RODS_SERVER
const char PAM_STACK_NAME[] = "irods";
const char PAM_CHECKER[] = "/usr/sbin/pam_handshake_auth_check";
const int SESSION_TIMEOUT = 3600;
#endif

static PamHandshake::Message::Context get_message_context(const irods::kvp_map_t & kvp)
{
  if (WhereAmI::getExecutableBaseName() == "iinit" ||
      WhereAmI::getExecutableBaseName() == "iinit.exe")
  {
    return PamHandshake::Message::Context::IInit;
  }
  else
  {
    return PamHandshake::Message::Context::ICommand;
  }
}


irods::error pam_auth_client_start(irods::plugin_context& _ctx,
                                   rcComm_t* _comm,
                                   const char* _context )
{
  irods::error result = SUCCESS();
  irods::error ret;
  // =-=-=-=-=-=-=-
  // validate incoming parameters
  ret = _ctx.valid<irods::generic_auth_object>();
  if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() )
  {
    if ( ( result = ASSERT_ERROR( _comm, SYS_INVALID_INPUT_PARAM, "Null comm pointer." ) ).ok() )
    {
      if(!_context)
      {
        _context = "";
      }
      if ( ( result = ASSERT_ERROR( _context, SYS_INVALID_INPUT_PARAM, "Null context pointer." ) ).ok() )
      {
        auto ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>(_ctx.fco());

        // =-=-=-=-=-=-=-
        // set the user name from the conn
        ptr->user_name( _comm->proxyUser.userName );

        // =-=-=-=-=-=-=-
        // set the zone name from the conn
        ptr->zone_name( _comm->proxyUser.rodsZone );
        ptr->context(_context);
        PAM_CLIENT_LOG(PAMLOG_DEBUG, "pam_auth_client_start " << ptr->context());
        PAM_CLIENT_LOG(PAMLOG_DEBUG, "verbose level " << VERBOSE_LEVEL);
      } // if context not null ptr
    } // if comm not null ptr
  }
  return result;
} // pam_auth_client_start


static std::tuple<int, std::string> pam_auth_get_session(rcComm_t* _comm)
{
  authPluginReqInp_t req_in;
  authPluginReqOut_t* req_out = 0;
  std::string res;
  std::string ctx_str = irods::escaped_kvp_string(irods::kvp_map_t{{"METHOD", "POST"}});
  strncpy(req_in.context_, ctx_str.c_str(), ctx_str.size() + 1 );
  strncpy(req_in.auth_scheme_,
          AUTH_PAM_INTERACTIVE_SCHEME,
          sizeof(AUTH_PAM_INTERACTIVE_SCHEME) + 1);
  int status = rcAuthPluginRequest( _comm, &req_in, &req_out );

  if(req_out)
  {
    res = req_out->result_;
    free(req_out);
  }
  if(status == 0)
  {
    irods::kvp_map_t kvp;
    irods::error ret = irods::parse_escaped_kvp_string(res, kvp);
    if ( !ret.ok() )
    {
      return std::make_tuple(SYS_INVALID_INPUT_PARAM, "cannot decode kvp");
    }
    {
      auto itr = kvp.find("CODE");
      if(itr == kvp.end())
      {
        return std::make_tuple(SYS_INVALID_INPUT_PARAM, "SESSION key missing");
      }
      if(itr->second != "200")
      {
        std::string msg = std::string("http code:") + itr->second;
        return std::make_tuple(-1, msg);
      }
    }
    {
      auto itr = kvp.find("SESSION");
      if(itr == kvp.end())
      {
        return std::make_tuple(SYS_INVALID_INPUT_PARAM, "SESSION key missing");
      }
      else
      {
        return std::make_tuple(0, itr->second);
      }
    }
  }
  else
  {
    return std::make_tuple(status, "request failed");
  }
}

static bool pam_auth_delete_session(rcComm_t* _comm, const std::string & session)
{
  authPluginReqInp_t req_in;
  authPluginReqOut_t* req_out = 0;
  std::string res;
  std::string ctx_str = irods::escaped_kvp_string(irods::kvp_map_t{
      {"METHOD", "DELETE"},
      {"SESSION", session}});
  strncpy(req_in.context_, ctx_str.c_str(), ctx_str.size() + 1 );
  strncpy(req_in.auth_scheme_,
          AUTH_PAM_INTERACTIVE_SCHEME,
          sizeof(AUTH_PAM_INTERACTIVE_SCHEME) + 1 );
  int status = rcAuthPluginRequest( _comm, &req_in, &req_out );
  if(req_out)
  {
    res = req_out->result_;
    free(req_out);
  }
  if(status == 0)
  {
     irods::kvp_map_t kvp;
     irods::error ret = irods::parse_escaped_kvp_string(res, kvp);
     if ( !ret.ok() )
     {
       return false; 
     }
     auto itr = kvp.find("CODE");
     if(itr == kvp.end() || itr->second != "200")
     {
       return false;
     }
  }
  else
  {
    return false;
  }
  return true;
}

void setSessionSignatureClientside(char* _sig);

irods::error pam_auth_client_request(irods::plugin_context& _ctx, rcComm_t* _comm )
{
  if(!_ctx.valid< irods::generic_auth_object>().ok())
  {
    return ERROR(SYS_INVALID_INPUT_PARAM, "invalid plugin context" );
  }
  else if(!_comm)
  {
    return ERROR(SYS_INVALID_INPUT_PARAM, "null comm ptr" );
  }
  else
  {
    auto ptr = boost::dynamic_pointer_cast <irods::generic_auth_object>(_ctx.fco());
    bool using_ssl = (irods::CS_NEG_USE_SSL == _comm->negotiation_results );
    irods::kvp_map_t kvp;
    PAM_CLIENT_LOG(PAMLOG_DEBUG, "pam_auth_client_start " << ptr->context());
    PAM_CLIENT_LOG(PAMLOG_DEBUG, "verbose level " << VERBOSE_LEVEL);
    irods::error ret = irods::parse_escaped_kvp_string(ptr->context(), kvp);
    if (!ret.ok())
    {
      return ret;
    }
    PamHandshake::Message::Context message_context = get_message_context(kvp);
    if ( !using_ssl )
    {
      PAM_CLIENT_LOG(PAMLOG_DEBUG, "sslStart");
      int err = sslStart( _comm );
      if ( err )
      {
        return ERROR( -1, "failed to enable ssl" );
      }
    }
    PamHandshake::Conversation conversation;
    try
    {
      conversation.load(VERBOSE_LEVEL);
    }
    catch(const std::exception & ex)
    {
      PAM_CLIENT_LOG(PAMLOG_INFO, "failed to load conversation file " << ex.what());
      conversation.reset();
    }
    std::string session;
    int status = 0;
    std::tie(status, session) = pam_auth_get_session(_comm);
    bool active = true;
    std::string answer;
    std::string err_msg;
    bool conversation_done = false;
    bool authenticated = false;
    while(active && (status == 0))
    {
      authPluginReqInp_t req_in;
      authPluginReqOut_t* req_out = 0;
      //@todo remove variable
      //irods::kvp_map_t kvp;
      std::string ctx_str = irods::escaped_kvp_string(irods::kvp_map_t{
          {"METHOD", "PUT"},
          {"SESSION", session},
          {"ANSWER", answer}});
      if(VERBOSE_LEVEL >= PAMLOG_DEBUG)
      {
        std::string dbg_ctx_str = irods::escaped_kvp_string(irods::kvp_map_t{
          {"METHOD", "PUT"},
          {"SESSION", session},
          {"ANSWER", "***"}});
        PAM_CLIENT_LOG(PAMLOG_DEBUG, "REQUEST:" << dbg_ctx_str);
      }

      if((ctx_str.size() + 1) > MAX_NAME_LEN)
      {
        std::cerr << "input lenght exceeded (" << ctx_str.size() << ">=" << MAX_NAME_LEN << ")"
                  << std::endl;
        status = SYS_BAD_INPUT;
        break;
      }
      strncpy(req_in.context_, ctx_str.c_str(), ctx_str.size() + 1 );
      strncpy(req_in.auth_scheme_,
              AUTH_PAM_INTERACTIVE_SCHEME,
              sizeof(AUTH_PAM_INTERACTIVE_SCHEME) + 1 );
      status = rcAuthPluginRequest( _comm, &req_in, &req_out );
      if(status < 0)
      {
        if(req_out)
        {
          free(req_out);
        }
        if(status == PAM_AUTH_PASSWORD_FAILED)
        {
          conversation_done = true;
          authenticated = false;
        }
        break;
      }
      try
      {
        PamHandshake::Message msg(std::string(req_out->result_));
        msg.applyPatch(conversation);
        switch(msg.getState())
        {
        case PamHandshake::Message::State::Waiting:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "Waiting");
          answer = msg.input(conversation, message_context);
          break;
        case PamHandshake::Message::State::WaitingPw:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "WaitingPw");
          answer = msg.input_password(conversation, message_context);
          break;
        case PamHandshake::Message::State::NotAuthenticated:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "NotAuthenticated");
          status = 0;
          active = false;
          conversation_done = true;
          authenticated = false;
          break;
        case PamHandshake::Message::State::Authenticated:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "Authenticated");
          status = 0;
          active = false;
          conversation_done = true;
          authenticated = true;
          break;
        case PamHandshake::Message::State::Error:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "Error");
          status = -1;
          active = false;
          err_msg = std::string("PAM error: ") + msg.getMessage();
          break;
        case PamHandshake::Message::State::Timeout:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "Timeout");
          status = -1;
          active = false;
          err_msg = std::string("PAM timeout");
          break;
        case PamHandshake::Message::State::Next:
          PAM_CLIENT_LOG(PAMLOG_DEBUG, "Next");
          msg.echo(message_context);
          break;
        default:
          status = -1;
          err_msg = std::string("invalid state");
          break;
        }
      }
      catch(const std::exception & ex)
      {
        PAM_CLIENT_LOG(PAMLOG_ERROR, ex.what());
        status = -1;
        break;
      }
    }
    if(!err_msg.empty())
    {
      PAM_CLIENT_LOG(PAMLOG_INFO, "CONVERSATION ERR MSG:" << err_msg);
    }
    if(!using_ssl )
    {
      PAM_CLIENT_LOG(PAMLOG_DEBUG, "SSL_END");
      sslEnd( _comm );
    }
    else
    {
      PAM_CLIENT_LOG(PAMLOG_DEBUG, "CONTINUE SSL");
    }
    if(status < 0 || !conversation_done)
    {
      PAM_CLIENT_LOG(PAMLOG_DEBUG,
                     "ERROR: " << err_msg <<
                     " status:" << status <<
                     " conversation done:" << conversation_done);
      if(status == 0)
      {
        status = -1;
      }
      return ERROR(status, err_msg.c_str());
    }
    else
    {
      PAM_CLIENT_LOG(PAMLOG_DEBUG, "CONVERSATION DONE");
      if(authenticated)
      {
        PAM_CLIENT_LOG(PAMLOG_DEBUG, "PAM AUTH CHECK SUCCESS");
        // =-=-=-=-=-=-=-
        // and cache the result in our auth object
        ptr->request_result(conversation.dump().c_str());
        try
        {
          conversation.save(VERBOSE_LEVEL, false);
        }
        catch(const std::exception & ex)
        {
          return ERROR(-1, ex.what());
        }
        return SUCCESS();
      }
      else
      {
        PAM_CLIENT_LOG(PAMLOG_DEBUG, "PAM AUTH CHECK FAILED");
        return ERROR( PAM_AUTH_PASSWORD_FAILED, "pam auth check failed" );
      }
      //free( req_out );
    }
  }
} // pam_auth_client_request

static std::string serialize_ordered(const nlohmann::json & j)
{
  std::set<std::string> ordered;
  for (auto& el : j.items())
  {
    ordered.insert(el.key());
  }
  std::stringstream ss;
  bool first = true;
  for(auto& n: ordered)
  {
    if(first)
    {
      first = false;
    }
    else
    {
      ss << ",";
    }
    if(j[n].is_string())
    {
      ss << nlohmann::json(n) << ":" << j[n];
    }
    else if(j[n].is_object() && j[n].contains("value"))
    {
      ss << nlohmann::json(n) << ":" << j[n]["value"];
    }
    else
    {
      ss << "null"_json;
    }
  }
  return ss.str();
}

irods::error pam_auth_establish_context(irods::plugin_context& _ctx )
{
  if(!_ctx.valid<irods::generic_auth_object>().ok())
  {
    return ERROR(SYS_INVALID_INPUT_PARAM, "invalid plugin context" );
  }
  auto ptr = boost::dynamic_pointer_cast <irods::generic_auth_object> (_ctx.fco());
  std::string request_result(serialize_ordered(nlohmann::json::parse(ptr->request_result())));
  std::size_t len = std::max(std::size_t(request_result.size() + 1),
                             std::size_t(16));
  char * md5_buf = (char*)malloc(len);
  memset(md5_buf, 0, len);
  strcpy(md5_buf, request_result.c_str());
  setSessionSignatureClientside(md5_buf);
  MD5_CTX context;
  MD5_Init( &context );
  MD5_Update(&context, ( unsigned char* )md5_buf, request_result.size());
  free(md5_buf);
  char digest[ RESPONSE_LEN + 2 ];
  MD5_Final((unsigned char* )digest, &context );
  for ( int i = 0; i < RESPONSE_LEN; ++i )
  {
    if ( digest[ i ] == '\0' )
    {
      digest[ i ]++;
    }
  }
  irods::kvp_map_t kvp;
  irods::error ret = irods::parse_escaped_kvp_string(ptr->context(), kvp);
  if (!ret.ok())
  {
    return ret;
  }
  kvp["DIGEST"] = digest;
  ptr->context(irods::escaped_kvp_string(kvp));
  return SUCCESS();
}

irods::error pam_auth_client_response(irods::plugin_context& _ctx,
                                      rcComm_t* _comm )
{
  irods::error result = SUCCESS();
  irods::error ret;
  // =-=-=-=-=-=-=-
  // validate incoming parameters
  ret = _ctx.valid<irods::generic_auth_object>();
  if ( ( result = ASSERT_PASS( ret, "Invalid plugin context." ) ).ok() )
  {
    if ( ( result = ASSERT_ERROR( _comm, SYS_INVALID_INPUT_PARAM, "Null rcComm_t pointer." ) ).ok() )
    {
      // =-=-=-=-=-=-=-
      // get the auth object
      auto ptr = boost::dynamic_pointer_cast<irods::generic_auth_object>( _ctx.fco() );
      std::string user_name = ptr->user_name() + "#" + ptr->zone_name();
      char username[ MAX_NAME_LEN ];
      snprintf( username, MAX_NAME_LEN, "%s", user_name.c_str() );
      authResponseInp_t auth_response;
      irods::kvp_map_t kvp;
      irods::error ret = irods::parse_escaped_kvp_string(ptr->context(), kvp);
      if (!ret.ok())
      {
        return ret;
      }
      auto itr = kvp.find("DIGEST");
      if(itr == kvp.end())
      {
        auth_response.response = "";
      }
      else
      {
        auth_response.response = const_cast<char*>(itr->second.c_str());
      }
      auth_response.username = username;
      int status = rcAuthResponse( _comm, &auth_response );
      result = ASSERT_ERROR( status >= 0, status, "Call to rcAuthResponseFailed." );
    }
  }
  return result;
}

#ifdef RODS_SERVER
irods::error pam_auth_agent_request(irods::plugin_context& _ctx )
{
  using Session = PamHandshake::Session;
  static const std::string empty_string;
  // @Todo
  // =-=-=-=-=-=-=-
  // validate incoming parameters
  if ( !_ctx.valid<irods::generic_auth_object>().ok() )
  {
    return ERROR( SYS_INVALID_INPUT_PARAM, "invalid plugin context" );
  }

  // =-=-=-=-=-=-=-
  // get the server host handle
  rodsServerHost_t* server_host = 0;
  int status = getAndConnRcatHost(_ctx.comm(),
                                  MASTER_RCAT,
                                  ( const char* )_ctx.comm()->clientUser.rodsZone,
                                  &server_host );
  if ( status < 0 )
  {
    return ERROR( status, "getAndConnRcatHost failed." );
  }
  
  auto ptr = boost::dynamic_pointer_cast <irods::generic_auth_object>(_ctx.fco());
  std::string context = ptr->context( );
  // =-=-=-=-=-=-=-
  // if we are not the catalog server, redirect the call
  // to there
  if ( server_host->localFlag != LOCAL_HOST )
  {
    // =-=-=-=-=-=-=-
    // protect the PAM plain text password by
    // using an SSL connection to the remote ICAT
    status = sslStart( server_host->conn );
    if ( status )
    {
      return ERROR( status, "could not establish SSL connection" );
    }
    // =-=-=-=-=-=-=-
    // manufacture structures for the redirected call
    authPluginReqOut_t* req_out = 0;
    authPluginReqInp_t  req_inp;
    strncpy( req_inp.auth_scheme_,
             AUTH_PAM_INTERACTIVE_SCHEME,
             sizeof(AUTH_PAM_INTERACTIVE_SCHEME) + 1 );
    strncpy( req_inp.context_,
             context.c_str(),
             context.size() + 1 );
    status = rcAuthPluginRequest( server_host->conn,
                                  &req_inp,
                                  &req_out );
    sslEnd( server_host->conn );
    rcDisconnect( server_host->conn );
    server_host->conn = NULL;
    if ( !req_out || status < 0 )
    {
      return ERROR( status, "redirected rcAuthPluginRequest failed." );
    }
    else
    {
      ptr->request_result( req_out->result_ );
      if ( _ctx.comm()->auth_scheme != NULL )
      {
        free( _ctx.comm()->auth_scheme );
      }
      _ctx.comm()->auth_scheme = strdup(AUTH_PAM_INTERACTIVE_SCHEME);
      return SUCCESS();
    }
  } // if !localhost
  irods::kvp_map_t kvp;
  irods::error ret = irods::parse_escaped_kvp_string(context, kvp);
  if ( !ret.ok() )
  {
    return PASS( ret );
  }
  try
  {
    auto itr = kvp.find("METHOD");
    if(itr == kvp.end())
    {
      return ERROR(SYS_INVALID_INPUT_PARAM, "METHOD key missing");
    }
    else if(itr->second == "POST")
    {
      auto session = Session::getSingleton(PAM_STACK_NAME,
                                           PAM_CHECKER,
                                           SESSION_TIMEOUT);
      
      ptr->request_result(irods::escaped_kvp_string(irods::kvp_map_t{
            {"SESSION", ""},
              {"CODE", "200"}}).c_str());
      return SUCCESS();
    }
    else
    {
      if(itr->second == "GET")
      {
        auto session = Session::getSingleton(PAM_STACK_NAME,
                                             PAM_CHECKER,
                                             SESSION_TIMEOUT);
        ptr->request_result(irods::escaped_kvp_string(irods::kvp_map_t{
              {"SESSION", ""},
              {"CODE", "200"},
              {"STATE", Session::StateToString(session->getState())},
              {"MESSAGE", ""}
            }).c_str());
        return SUCCESS();
      }
      else if(itr->second == "PUT")
      {
        auto aitr = kvp.find("ANSWER");
        const std::string & answer((aitr == kvp.end()) ? empty_string : aitr->second);
        auto session = Session::getSingleton(PAM_STACK_NAME,
                                             PAM_CHECKER,
                                             SESSION_TIMEOUT);
        auto p = session->pull(answer.c_str(),
                               answer.size());
        int http_code = 200;
        if(p.first == Session::State::NotAuthenticated)
        {
          http_code = 401;
        }
        else if(p.first == Session::State::Authenticated)
        {
          http_code = 202;
        }
        else if(p.first == Session::State::Error)
        {
          http_code = 500;
        }
        if(p.first == Session::State::NotAuthenticated || 
           p.first == Session::State::Authenticated ||
           p.first == Session::State::Error ||
           p.first == Session::State::Timeout)
        {
          PamHandshake::Session::resetSingleton();
        }
        ptr->request_result(irods::escaped_kvp_string(irods::kvp_map_t{
              {"SESSION", ""},
              {"CODE", std::to_string(http_code)},
              {"STATE", Session::StateToString(p.first)},
              {"MESSAGE", p.second}
            }).c_str());
        if(p.first == Session::State::NotAuthenticated)
        {
          return ERROR(PAM_AUTH_PASSWORD_FAILED, "pam auth check failed" );
        }
        else if(p.first == Session::State::Authenticated)
        {
          return SUCCESS();
        }
        else if(p.first == Session::State::Error ||
                p.first == Session::State::Timeout)
        {
          return ERROR( -1,
                        (std::string("pam aux service failure ") +
                         Session::StateToString(p.first) +
                         std::string(" ") +
                         p.second).c_str());
        }
        else
        {
          return SUCCESS();
        }
      }
      else if(itr->second == "DELETE")
      {
        PamHandshake::Session::resetSingleton();
        ptr->request_result(irods::escaped_kvp_string(irods::kvp_map_t{
              {"SESSION", ""},
              {"CODE", std::to_string(200)}}).c_str());
        return SUCCESS();
      }
      else
      {
        std::string msg("invalid METHOD '");
        msg+= itr->second;
        msg+= "'";
        return ERROR(SYS_INVALID_INPUT_PARAM, msg.c_str());
      }
    }
  }
  catch(const std::exception & ex)
  {
    //@todo error handling
    rodsLog(LOG_ERROR, "open_pam_handshake_session: %s", ex.what());
    return ERROR( -1, ex.what() );
  }
} // pam_auth_agent_request
#endif

#ifdef RODS_SERVER
irods::error pam_auth_agent_start(irods::plugin_context&, const char*)
{
    return SUCCESS();
}
#endif

#ifdef RODS_SERVER
// copied code from lib native. Need better solution.
//const static int requireServerAuth = 0;
//const static int requireSIDs = 0;
//void _rsSetAuthRequestGetChallenge( const char* _c );

static irods::error check_proxy_user_privileges(
    rsComm_t *rsComm,
    int proxyUserPriv ) {
    irods::error result = SUCCESS();

    if ( strcmp( rsComm->proxyUser.userName, rsComm->clientUser.userName ) != 0 ) {

        /* remote privileged user can only do things on behalf of users from
         * the same zone */
        result = ASSERT_ERROR( proxyUserPriv >= LOCAL_PRIV_USER_AUTH ||
                               ( proxyUserPriv >= REMOTE_PRIV_USER_AUTH &&
                                 strcmp( rsComm->proxyUser.rodsZone, rsComm->clientUser.rodsZone ) == 0 ),
                               SYS_PROXYUSER_NO_PRIV,
                               "Proxyuser: \"%s\" with %d no priv to auth clientUser: \"%s\".",
                               rsComm->proxyUser.userName, proxyUserPriv, rsComm->clientUser.userName );
    }

    return result;
}

irods::error pam_auth_agent_response(irods::plugin_context& _ctx, authResponseInp_t* _resp )
{
  irods::error ret = SUCCESS();
  ret = _ctx.valid();
  if ( !ret.ok() )
  {
    return PASSMSG( "Invalid plugin context.", ret );
  }
  if ( NULL == _resp )
  {
    return ERROR( SYS_INVALID_INPUT_PARAM, "Invalid response or comm pointers." );
  }
  authCheckInp_t authCheckInp;
  authCheckOut_t *authCheckOut = NULL;
  rodsServerHost_t *rodsServerHost;
  int status;
  memset( &authCheckInp, 0, sizeof( authCheckInp ) );
  status = getAndConnRcatHostNoLogin(_ctx.comm(),
                                     MASTER_RCAT,
                                     _ctx.comm()->proxyUser.rodsZone,
                                     &rodsServerHost );
  if ( status < 0 )
  {
    return ERROR( status, "Connecting to rcat host failed." );
  }
  std::string response =
    irods::AUTH_SCHEME_KEY +
    irods::kvp_association() +
    AUTH_PAM_INTERACTIVE_SCHEME +
    irods::kvp_delimiter() +
    irods::AUTH_RESPONSE_KEY +
    irods::kvp_association() +
    _resp->response;
  authCheckInp.response = const_cast<char*>(response.c_str());
  authCheckInp.username = _resp->username;
  authCheckInp.challenge = "dummy";
  if ( LOCAL_HOST == rodsServerHost->localFlag )
  {
    status = rsAuthCheck( _ctx.comm(), &authCheckInp, &authCheckOut );
  }
  else
  {
    status = rcAuthCheck( rodsServerHost->conn, &authCheckInp, &authCheckOut );
    /* not likely we need this connection again */
    rcDisconnect( rodsServerHost->conn );
    rodsServerHost->conn = NULL;
  }
  if(status < 0)
  {
    free( authCheckOut->serverResponse );
    free( authCheckOut );
    return ERROR(status, "rcAuthCheck failed");
  }

  /* have to modify privLevel if the icat is a foreign icat because
   * a local user in a foreign zone is not a local user in this zone
   * and vice versa for a remote user
   */
  if ( rodsServerHost->rcatEnabled == REMOTE_ICAT )
  {
    /* proxy is easy because rodsServerHost is based on proxy user */
    if ( authCheckOut->privLevel == LOCAL_PRIV_USER_AUTH)
    {
      authCheckOut->privLevel = REMOTE_PRIV_USER_AUTH;
    }
    else if ( authCheckOut->privLevel == LOCAL_USER_AUTH )
    {
      authCheckOut->privLevel = REMOTE_USER_AUTH;
    }

    /* adjust client user */
    if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) == 0 )
    {
      authCheckOut->clientPrivLevel = authCheckOut->privLevel;
    }
    else
    {
      zoneInfo_t *tmpZoneInfo;
      status = getLocalZoneInfo( &tmpZoneInfo );
      if ( status < 0 )
      {
        free( authCheckOut->serverResponse );
        free( authCheckOut );
        return ERROR(status, "getLocalZoneInfo failed" );
      }
      if ( strcmp( tmpZoneInfo->zoneName,  _ctx.comm()->clientUser.rodsZone ) == 0 )
      {
        /* client is from local zone */
        if ( authCheckOut->clientPrivLevel == REMOTE_PRIV_USER_AUTH )
        {
          authCheckOut->clientPrivLevel = LOCAL_PRIV_USER_AUTH;
        }
        else if ( authCheckOut->clientPrivLevel == REMOTE_USER_AUTH )
        {
          authCheckOut->clientPrivLevel = LOCAL_USER_AUTH;
        }
      }
      else
      {
        /* client is from remote zone */
        if ( authCheckOut->clientPrivLevel == LOCAL_PRIV_USER_AUTH )
        {
          authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
        }
        else if ( authCheckOut->clientPrivLevel == LOCAL_USER_AUTH )
        {
          authCheckOut->clientPrivLevel = REMOTE_USER_AUTH;
        }
      }
    }
  }
  else if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) == 0 )
  {
    authCheckOut->clientPrivLevel = authCheckOut->privLevel;
  }

  if ( strcmp( _ctx.comm()->proxyUser.userName,  _ctx.comm()->clientUser.userName ) != 0 )
  {
    _ctx.comm()->proxyUser.authInfo.authFlag = authCheckOut->privLevel;
    _ctx.comm()->clientUser.authInfo.authFlag = authCheckOut->clientPrivLevel;
  }
  else
  {
    /* proxyUser and clientUser are the same */
    _ctx.comm()->proxyUser.authInfo.authFlag =
      _ctx.comm()->clientUser.authInfo.authFlag = authCheckOut->privLevel;
  }
  free( authCheckOut->serverResponse );
  free( authCheckOut );
  return SUCCESS();
}
#endif

#ifdef RODS_SERVER
irods::error pam_auth_agent_verify(irods::plugin_context& ,
                                   const char* _challenge,
                                   const char* _user_name,
                                   const char* _response)
{
  //@todo
  return SUCCESS();
}
#endif

// =-=-=-=-=-=-=-
// derive a new pam_auth auth plugin from
// the auth plugin base class for handling
// native authentication
class pam_interactive_auth_plugin : public irods::auth {
    public:
        pam_interactive_auth_plugin(
            const std::string& _nm,
            const std::string& _ctx ) :
            irods::auth(
                _nm,
                _ctx ) {
        } // ctor

        ~pam_interactive_auth_plugin() {
        }

}; // class pam_auth_plugin

// =-=-=-=-=-=-=-
// factory function to provide instance of the plugin
extern "C"
irods::auth* plugin_factory(
    const std::string& _inst_name,
    const std::string& _context ) {
#ifdef RODS_SERVER
  //std::cout << "plugin_factory XXXXXXXXXXXXXXXXX " << getpid() << std::endl;
#endif
    // =-=-=-=-=-=-=-
    // create an auth object
    pam_interactive_auth_plugin* pam = new pam_interactive_auth_plugin(
        _inst_name,
        _context );

    // =-=-=-=-=-=-=-
    // fill in the operation table mapping call
    // names to function names
    using namespace irods;
    using namespace std;
    pam->add_operation(
        AUTH_ESTABLISH_CONTEXT,
        function<error(plugin_context&)>(
            pam_auth_establish_context ) );
    pam->add_operation<rcComm_t*,const char*>(
        AUTH_CLIENT_START,
        function<error(plugin_context&,rcComm_t*,const char*)>(
            pam_auth_client_start ) );
    pam->add_operation<rcComm_t*>(
        AUTH_CLIENT_AUTH_REQUEST,
        function<error(plugin_context&,rcComm_t*)>(
            pam_auth_client_request ) );
    pam->add_operation<rcComm_t*>(
        AUTH_CLIENT_AUTH_RESPONSE,
        function<error(plugin_context&,rcComm_t*)>(
            pam_auth_client_response ) );
#ifdef RODS_SERVER
    pam->add_operation(
        AUTH_AGENT_START,
        function<error(plugin_context&,const char*)>(
            pam_auth_agent_start ) );

    pam->add_operation(
        AUTH_AGENT_AUTH_REQUEST,
        function<error(plugin_context&)>(
            pam_auth_agent_request )  );
    pam->add_operation<authResponseInp_t*>(
        AUTH_AGENT_AUTH_RESPONSE,
        function<error(plugin_context&,authResponseInp_t*)>(
            pam_auth_agent_response ) );
    pam->add_operation<const char*,const char*,const char*>(
        AUTH_AGENT_AUTH_VERIFY,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            pam_auth_agent_verify ) );
#endif
    irods::auth* auth = dynamic_cast< irods::auth* >( pam );

    return auth;

} // plugin_factory
