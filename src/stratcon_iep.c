/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "noit_defines.h"
#include "eventer/eventer.h"
#include "utils/noit_log.h"
#include "utils/noit_b64.h"
#include "noit_jlog_listener.h"
#include "stratcon_jlog_streamer.h"
#include "stratcon_datastore.h"
#include "stratcon_iep.h"
#include "noit_conf.h"
#include "noit_check.h"
#include "noit_xml.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/fcntl.h>
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#include <assert.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#ifdef OPENWIRE
#include "amqcs.h"
#else
#include "stomp/stomp.h"
#endif

eventer_jobq_t iep_jobq;
static noit_log_stream_t noit_iep = NULL;

struct iep_thread_driver {
#ifdef OPENWIRE
  amqcs_connect_options connect_options;
  amqcs_connection *connection;
#else
  stomp_connection *connection;
#endif
  apr_pool_t *pool;
  char* exchange;
};
pthread_key_t iep_connection;

struct iep_job_closure {
  char *line;       /* This is a copy and gets trashed during processing */
  char *remote;
  xmlDocPtr doc;
  char *doc_str;
  apr_pool_t *pool;
};

static void
start_iep_daemon();

static int
bust_to_parts(char *in, char **p, int len) {
  int cnt = 0;
  char *s = in;
  while(cnt < len) {
    p[cnt++] = s;
    while(*s && *s != '\t') s++;
    if(!*s) break;
    *s++ = '\0';
  }
  while(*s) s++; /* Move to end */
  if(s > in && *(s-1) == '\n') *(s-1) = '\0'; /* chomp */
  return cnt;
}

#define ADDCHILD(a,b) \
  xmlNewTextChild(root, NULL, (xmlChar *)(a), (xmlChar *)(b))
#define NEWDOC(xmldoc,n,stanza) do { \
  xmlNodePtr root; \
  xmldoc = xmlNewDoc((xmlChar *)"1.0"); \
  root = xmlNewDocNode(xmldoc, NULL, (xmlChar *)(n), NULL); \
  xmlDocSetRootElement(xmldoc, root); \
  stanza \
} while(0)


static xmlDocPtr
stratcon_iep_doc_from_status(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[7];
  if(bust_to_parts(data, parts, 7) != 7) return NULL;
  /* 'S' TIMESTAMP UUID STATE AVAILABILITY DURATION STATUS_MESSAGE */
  NEWDOC(doc, "NoitStatus",
         {
           ADDCHILD("remote", remote);
           ADDCHILD("id", parts[2]);
           ADDCHILD("state", parts[3]);
           ADDCHILD("availability", parts[4]);
           ADDCHILD("duration", parts[5]);
           ADDCHILD("status", parts[6]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_check(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[6];
  if(bust_to_parts(data, parts, 6) != 6) return NULL;
  /* 'C' TIMESTAMP UUID TARGET MODULE NAME */
  NEWDOC(doc, "NoitCheck",
         {
           ADDCHILD("remote", remote);
           ADDCHILD("id", parts[2]);
           ADDCHILD("target", parts[3]);
           ADDCHILD("module", parts[4]);
           ADDCHILD("name", parts[5]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_metric(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[6];
  const char *rootname = "NoitMetricNumeric";
  const char *valuename = "value";
  if(bust_to_parts(data, parts, 6) != 6) return NULL;
  /*  'M' TIMESTAMP UUID NAME TYPE VALUE */

  if(*parts[4] == METRIC_STRING) {
    rootname = "NoitMetricText";
    valuename = "message";
  }
  NEWDOC(doc, rootname,
         {
           ADDCHILD("remote", remote);
           ADDCHILD("id", parts[2]);
           ADDCHILD("name", parts[3]);
           ADDCHILD(valuename, parts[5]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_statement(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[3];
  if(bust_to_parts(data, parts, 3) != 3) return NULL;
  /*  'D' ID QUERY  */

  NEWDOC(doc, "StratconStatement",
         {
           ADDCHILD("id", parts[1]);
           ADDCHILD("expression", parts[2]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_query(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[4];
  if(bust_to_parts(data, parts, 4) != 4) return NULL;
  /*  'Q' ID NAME QUERY  */

  NEWDOC(doc, "StratconQuery",
         {
           ADDCHILD("id", parts[1]);
           ADDCHILD("name", parts[2]);
           ADDCHILD("expression", parts[3]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_querystop(char *data, char *remote) {
  xmlDocPtr doc;
  char *parts[2];
  if(bust_to_parts(data, parts, 2) != 2) return NULL;
  /*  'Q' ID */

  NEWDOC(doc, "StratconQueryStop",
         {
           xmlNodeSetContent(root, (xmlChar *)parts[1]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_line(char *data, char *remote) {
  if(data) {
    switch(*data) {
      case 'C': return stratcon_iep_doc_from_check(data, remote);
      case 'S': return stratcon_iep_doc_from_status(data, remote);
      case 'M': return stratcon_iep_doc_from_metric(data, remote);
      case 'D': return stratcon_iep_doc_from_statement(data, remote);
      case 'Q': return stratcon_iep_doc_from_query(data, remote);
      case 'q': return stratcon_iep_doc_from_querystop(data, remote);
    }
  }
  return NULL;
}

static float
stratcon_iep_age_from_line(char *data, struct timeval now) {
  float n, t;
  if(data && (*data == 'S' || *data == 'M')) {
    if(data[1] != '\t') return 0;
    t = strtof(data + 2, NULL);
    n = (float)now.tv_sec + (float)now.tv_usec / 1000000.0;
    return n - t;
  }
  return 0;
}

struct statement_node {
  char *id;
  char *statement;
  char *provides;
  int marked; /* helps with identifying cycles */
  int nrequires;
  struct statement_node **requires;
};
static void
statement_node_free(void *vstmt) {
  struct statement_node *stmt = vstmt;
  if(stmt->id) free(stmt->id);
  if(stmt->statement) free(stmt->statement);
  if(stmt->provides) free(stmt->provides);
  if(stmt->requires) free(stmt->requires);
}
static int
stmt_mark_dag(struct statement_node *stmt, int mgen) {
  int i;
  assert(stmt->marked <= mgen);
  if(stmt->marked == mgen) return -1;
  if(stmt->marked > 0) return 0; /* validated in a previous sweep */
  stmt->marked = mgen;
  for(i=0; i<stmt->nrequires; i++)
    if(stmt_mark_dag(stmt->requires[i], mgen) < 0) return -1;
  return 0;
}
static void
submit_statement_node(struct statement_node *stmt) {
  int line_len, i;
  char *line, *cp;

  if(stmt->marked) return;
  for(i=0; i<stmt->nrequires; i++)
    submit_statement_node(stmt->requires[i]);

  line_len = 3 /* 2 tabs + \0 */ +
             1 /* 'D' */ + 1 /* '\n' */ +
             strlen(stmt->id) + strlen(stmt->statement);
  line = malloc(line_len);
  snprintf(line, line_len, "D\t%s\t%s\n", stmt->id, stmt->statement);
  cp = line;
  while(cp[0] && cp[1]) {
    if(*cp == '\n') *cp = ' ';
    cp++;
  }
  noitL(noit_error, "submitting statement: %s\n", line);
  stratcon_iep_line_processor(DS_OP_INSERT, NULL, line);
  stmt->marked = 1;
}
void stratcon_iep_submit_statements() {
  int i, cnt = 0;
  noit_conf_section_t *statement_configs;
  char path[256];
  struct statement_node *stmt;
  void *vstmt;
  noit_hash_table stmt_by_id = NOIT_HASH_EMPTY;
  noit_hash_table stmt_by_provider = NOIT_HASH_EMPTY;
  noit_hash_iter iter = NOIT_HASH_ITER_ZERO;
  const char *key;
  int klen, mgen = 0;

  snprintf(path, sizeof(path), "/stratcon/iep/queries//statement");
  statement_configs = noit_conf_get_sections(NULL, path, &cnt);
  noitL(noit_debug, "Found %d %s stanzas\n", cnt, path);

  /* Phase 1: sweep in all the statements */
  for(i=0; i<cnt; i++) {
    char id[UUID_STR_LEN];
    char provides[256];
    char *statement;

    if(!noit_conf_get_stringbuf(statement_configs[i],
                                "self::node()/@id",
                                id, sizeof(id))) {
      noitL(noit_iep, "No uuid specified in query\n");
      continue;
    }
    if(!noit_conf_get_stringbuf(statement_configs[i],
                                "ancestor-or-self::node()/@provides",
                                provides, sizeof(provides))) {
      provides[0] = '\0';
    }
    if(!noit_conf_get_string(statement_configs[i], "self::node()/epl",
                             &statement)) {
      noitL(noit_iep, "No contents specified in statement\n");
      continue;
    }
    stmt = calloc(1, sizeof(*stmt));
    stmt->id = strdup(id);
    stmt->statement = statement;
    stmt->provides = provides[0] ? strdup(provides) : NULL;
    if(!noit_hash_store(&stmt_by_id, stmt->id, strlen(stmt->id), stmt)) {
      noitL(noit_error, "Duplicate statement id: %s\n", stmt->id);
      exit(-1);
    }
    if(stmt->provides) {
      if(!noit_hash_store(&stmt_by_provider, stmt->provides,
                          strlen(stmt->provides), stmt)) {
        noitL(noit_error, "Two statements provide: '%s'\n", stmt->provides);
        exit(-1);
      }
    }
  }

  /* Phase 2: load the requires graph */
  for(i=0; i<cnt; i++) {
    char id[UUID_STR_LEN];
    int rcnt, j;
    char *requires;
    noit_conf_section_t *reqs;

    if(!noit_conf_get_stringbuf(statement_configs[i],
                                "self::node()/@id",
                                id, sizeof(id))) {
      noitL(noit_iep, "No uuid specified in query\n");
      continue;
    }
    if(!noit_hash_retrieve(&stmt_by_id, id, strlen(id), &vstmt)) {
      noitL(noit_error, "Cannot find statement: %s\n", id);
      exit(-1);
    }
    stmt = vstmt;
    reqs = noit_conf_get_sections(statement_configs[i],
                                  "self::node()/requires", &rcnt);
    if(rcnt > 0) {
      stmt->requires = malloc(rcnt * sizeof(*(stmt->requires)));
      for(j=0; j<rcnt; j++) {
        void *vrstmt;
        if(!noit_conf_get_string(reqs[j], "self::node()",
                                 &requires) || requires[0] == '\0') {
          continue;
        }
        if(!noit_hash_retrieve(&stmt_by_provider, requires, strlen(requires),
                               &vrstmt)) {
          noitL(noit_error,
                "Statement %s requires %s which no one provides.\n",
                stmt->id, requires);
          exit(-1);
        }
        stmt->requires[stmt->nrequires++] = vrstmt;
      }
    }
  }

  /* Phase 3: Recursive sweep and mark to detect cycles.
     We're walking the graph backwards here from dependent to provider,
     but a cycle is a cycle, so this validates the graph. */
  while(noit_hash_next(&stmt_by_id, &iter, &key, &klen, &vstmt)) {
    stmt = vstmt;
    if(stmt_mark_dag(stmt, ++mgen) < 0) {
      noitL(noit_error, "Statement %s has a cyclic requirement\n", stmt->id);
      exit(-1);
    }
  }

  /* Phase 4: clean the markings */
  mgen = 0;
  memset(&iter, 0, sizeof(iter));
  while(noit_hash_next(&stmt_by_id, &iter, &key, &klen, &vstmt)) {
    stmt = vstmt;
    stmt->marked = 0;
  }

  /* Phase 5: do the load */
  memset(&iter, 0, sizeof(iter));
  while(noit_hash_next(&stmt_by_id, &iter, &key, &klen, &vstmt)) {
    stmt = vstmt;
    submit_statement_node(stmt);
  }

  noit_hash_destroy(&stmt_by_provider, NULL, NULL);
  noit_hash_destroy(&stmt_by_id, NULL, statement_node_free);
  free(statement_configs);
}

void stratcon_iep_submit_queries() {
  int i, cnt = 0;
  noit_conf_section_t *query_configs;
  char path[256];

  snprintf(path, sizeof(path), "/stratcon/iep/queries//query");
  query_configs = noit_conf_get_sections(NULL, path, &cnt);
  noitL(noit_debug, "Found %d %s stanzas\n", cnt, path);
  for(i=0; i<cnt; i++) {
    char id[UUID_STR_LEN];
    char topic[256];
    char *query;
    char *line;
    int line_len;

    if(!noit_conf_get_stringbuf(query_configs[i],
                                "self::node()/@id",
                                id, sizeof(id))) {
      noitL(noit_iep, "No uuid specified in query\n");
      continue;
    }
    if(!noit_conf_get_stringbuf(query_configs[i],
                                "ancestor-or-self::node()/@topic",
                                topic, sizeof(topic))) {
      noitL(noit_iep, "No topic specified in query\n");
      continue;
    }
    if(!noit_conf_get_string(query_configs[i], "self::node()/epl",
                             &query)) {
      noitL(noit_iep, "No contents specified in query\n");
      continue;
    }
    line_len = 4 /* 3 tabs + \0 */ +
               1 /* 'Q' */ + 1 /* '\n' */ +
               strlen(id) + strlen(topic) + strlen(query);
    line = malloc(line_len);
    snprintf(line, line_len, "Q\t%s\t%s\t%s\n", id, topic, query);
    free(query);
    query = line;
    while(query[0] && query[1]) {
      if(*query == '\n') *query = ' ';
      query++;
    }
    stratcon_iep_line_processor(DS_OP_INSERT, NULL, line);
  }
  free(query_configs);
}

static
struct iep_thread_driver *stratcon_iep_get_connection() {
  apr_status_t rc;
  struct iep_thread_driver *driver;
  driver = pthread_getspecific(iep_connection);
  if(!driver) {
    driver = calloc(1, sizeof(*driver));
    pthread_setspecific(iep_connection, driver);
  }

  if(!driver->pool) {
    if(apr_pool_create(&driver->pool, NULL) != APR_SUCCESS) return NULL;
  }

  if(!driver->connection) {
    int port;
    char hostname[128];
    if(!noit_conf_get_int(NULL, "/stratcon/iep/stomp/port", &port))
      port = 61613;
    if(!noit_conf_get_stringbuf(NULL, "/stratcon/iep/stomp/hostname",
                                hostname, sizeof(hostname)))
      strlcpy(hostname, "127.0.0.1", sizeof(hostname));
#ifdef OPENWIRE
    memset(&driver->connect_options, 0, sizeof(driver->connect_options));
    strlcpy(driver->connect_options.hostname, hostname,
            sizeof(driver->connect_options.hostname));
    driver->connect_options.port = port;
    if(amqcs_connect(&driver->connection, &driver->connect_options,
                     driver->pool) != APR_SUCCESS) {
      noitL(noit_error, "MQ connection failed\n");
      return NULL;
    }
#else
    if(stomp_connect(&driver->connection, hostname, port,
                     driver->pool)!= APR_SUCCESS) {
      noitL(noit_error, "MQ connection failed\n");
      stomp_disconnect(&driver->connection);
      return NULL;
    }

    {
      stomp_frame frame;
      char username[128];
      char password[128];
      char* exchange = malloc(128);
      frame.command = "CONNECT";
      frame.headers = apr_hash_make(driver->pool);
      // This is for RabbitMQ Support
      if((noit_conf_get_stringbuf(NULL, "/stratcon/iep/stomp/username",
                                  username, sizeof(username))) &&
         (noit_conf_get_stringbuf(NULL, "/stratcon/iep/stomp/password",
                                  password, sizeof(password))))
      {
        apr_hash_set(frame.headers, "login", APR_HASH_KEY_STRING, username);
        apr_hash_set(frame.headers, "passcode", APR_HASH_KEY_STRING, password);
      }


      // This is for RabbitMQ support
      driver->exchange = NULL;
      if(noit_conf_get_stringbuf(NULL, "/stratcon/iep/stomp/exchange",
                                  exchange, 128))
      {
        if (!driver->exchange)
          driver->exchange = exchange;
        else
          free(exchange);
        apr_hash_set(frame.headers, "exchange", APR_HASH_KEY_STRING, driver->exchange);
      }



/*
      We don't use login/pass
      apr_hash_set(frame.headers, "login", APR_HASH_KEY_STRING, "");
      apr_hash_set(frame.headers, "passcode", APR_HASH_KEY_STRING, "");
*/
      frame.body = NULL;
      frame.body_length = -1;
      rc = stomp_write(driver->connection, &frame, driver->pool);
      if(rc != APR_SUCCESS) {
        noitL(noit_error, "MQ STOMP CONNECT failed, %d\n", rc);
        stomp_disconnect(&driver->connection);
        return NULL;
      }
    }  
    {
      stomp_frame *frame;
      rc = stomp_read(driver->connection, &frame, driver->pool);
      if (rc != APR_SUCCESS) {
        noitL(noit_error, "MQ STOMP CONNECT bad response, %d\n", rc);
        stomp_disconnect(&driver->connection);
        return NULL;
      }
     }
#endif
     stratcon_iep_submit_statements();
     stratcon_datastore_iep_check_preload();
     stratcon_iep_submit_queries();
  }

  return driver;
}

static int
setup_iep_connection_callback(eventer_t e, int mask, void *closure,
                              struct timeval *now) {
  stratcon_iep_line_processor(DS_OP_INSERT, NULL, NULL);
  return 0;
}

static void
setup_iep_connection_later(int seconds) {
  eventer_t newe = eventer_alloc();
  gettimeofday(&newe->whence, NULL);
  newe->whence.tv_sec += seconds;
  newe->mask = EVENTER_TIMER;
  newe->callback = setup_iep_connection_callback;
  newe->closure = NULL;
  eventer_add(newe);
}

static int
stratcon_iep_submitter(eventer_t e, int mask, void *closure,
                       struct timeval *now) {
  float age;
  struct iep_job_closure *job = closure;
  struct iep_thread_driver *driver;
  /* We only play when it is an asynch event */
  if(!(mask & EVENTER_ASYNCH_WORK)) return 0;

  if(mask & EVENTER_ASYNCH_CLEANUP) {
    /* free all the memory associated with the batch */
    if(job) {
      if(job->line) free(job->line);
      if(job->remote) free(job->remote);
      if(job->doc_str) free(job->doc_str);
      if(job->doc) xmlFreeDoc(job->doc);
      if(job->pool) apr_pool_destroy(job->pool);
      free(job);
    }
    return 0;
  }
  driver = stratcon_iep_get_connection();
  if(!driver) setup_iep_connection_later(1);

  if(!job->line || job->line[0] == '\0') return 0;

  if((age = stratcon_iep_age_from_line(job->line, *now)) > 60) {
    noitL(noit_debug, "Skipping old event %f second old.\n", age);
    return 0;
  }
  job->doc = stratcon_iep_doc_from_line(job->line, job->remote);
  if(job->doc) {
    job->doc_str = noit_xmlSaveToBuffer(job->doc);
    if(job->doc_str) {
      /* Submit */
      if(driver && driver->pool && driver->connection) {
        apr_status_t rc;
#ifdef OPENWIRE
        ow_ActiveMQQueue *dest;
        ow_ActiveMQTextMessage *message;

        apr_pool_create(&job->pool, driver->pool);
        message = ow_ActiveMQTextMessage_create(job->pool);
        message->content =
          ow_byte_array_create_with_data(job->pool,strlen(job->doc_str),
                                         job->doc_str);
        dest = ow_ActiveMQQueue_create(job->pool);
        dest->physicalName = ow_string_create_from_cstring(job->pool,"TEST.QUEUE");         
        rc = amqcs_send(driver->connection,
                        (ow_ActiveMQDestination*)dest,
                        (ow_ActiveMQMessage*)message,
                        1,4,0,job->pool);
        if(rc != APR_SUCCESS) {
          noitL(noit_error, "MQ send failed, disconnecting\n");
          if(driver->connection) amqcs_disconnect(&driver->connection);
          driver->connection = NULL;
        }
#else
        stomp_frame out;

        apr_pool_create(&job->pool, driver->pool);

        out.command = "SEND";
        out.headers = apr_hash_make(job->pool);
        if (driver->exchange)
          apr_hash_set(out.headers, "exchange", APR_HASH_KEY_STRING, driver->exchange);

        apr_hash_set(out.headers, "destination", APR_HASH_KEY_STRING, "/queue/noit.firehose");
        apr_hash_set(out.headers, "ack", APR_HASH_KEY_STRING, "auto");
      
        out.body_length = -1;
        out.body = job->doc_str;
        rc = stomp_write(driver->connection, &out, job->pool);
        if(rc != APR_SUCCESS) {
          noitL(noit_error, "STOMP send failed, disconnecting\n");
          if(driver->connection) stomp_disconnect(&driver->connection);
          driver->connection = NULL;
        }
#endif
      }
      else {
        noitL(noit_error, "Not submitting event, no MQ\n");
      }
    }
  }
  else {
    noitL(noit_iep, "no iep handler for: '%s'\n", job->line);
  }
  return 0;
}

void
stratcon_iep_line_processor(stratcon_datastore_op_t op,
                            struct sockaddr *remote, void *operand) {
  int len;
  char remote_str[128];
  struct iep_job_closure *jc;
  eventer_t newe;
  struct timeval __now, iep_timeout = { 20L, 0L };
  /* We only care about inserts */

  if(op == DS_OP_CHKPT) {
    eventer_add((eventer_t) operand);
    return;
  }
  if(op != DS_OP_INSERT) return;

  snprintf(remote_str, sizeof(remote_str), "%s", "0.0.0.0");
  if(remote) {
    switch(remote->sa_family) {
      case AF_INET:
        len = sizeof(struct sockaddr_in);
        inet_ntop(remote->sa_family, &((struct sockaddr_in *)remote)->sin_addr,
                  remote_str, len);
        break;
      case AF_INET6:
       len = sizeof(struct sockaddr_in6);
        inet_ntop(remote->sa_family, &((struct sockaddr_in6 *)remote)->sin6_addr,
                  remote_str, len);
       break;
      case AF_UNIX:
        len = SUN_LEN(((struct sockaddr_un *)remote));
        snprintf(remote_str, sizeof(remote_str), "%s", ((struct sockaddr_un *)remote)->sun_path);
        break;
    }
  }

  /* process operand and push onto queue */
  gettimeofday(&__now, NULL);
  newe = eventer_alloc();
  newe->mask = EVENTER_ASYNCH;
  add_timeval(__now, iep_timeout, &newe->whence);
  newe->callback = stratcon_iep_submitter;
  jc = calloc(1, sizeof(*jc));
  jc->line = operand;
  jc->remote = strdup(remote_str);
  newe->closure = jc;

  eventer_add_asynch(&iep_jobq, newe);
}

static void connection_destroy(void *vd) {
  struct iep_thread_driver *driver = vd;
#ifdef OPENWIRE
  if(driver->connection) amqcs_disconnect(&driver->connection);
#else
  if(driver->connection) stomp_disconnect(&driver->connection);
  if(driver->exchange) free(driver->exchange);
#endif
  if(driver->pool) apr_pool_destroy(driver->pool);
  free(driver);
}

jlog_streamer_ctx_t *
stratcon_jlog_streamer_iep_ctx_alloc(void) {
  jlog_streamer_ctx_t *ctx;
  ctx = stratcon_jlog_streamer_ctx_alloc();
  ctx->jlog_feed_cmd = htonl(NOIT_JLOG_DATA_TEMP_FEED);
  ctx->push = stratcon_iep_line_processor;
  return ctx;
}

struct iep_daemon_info {
  pid_t child;
  int stdin_pipe[2];
  int stderr_pipe[2];
  char *directory;
  char *command;
};

static void
iep_daemon_info_free(struct iep_daemon_info *info) {
  if(!info) return;
  if(info->directory) free(info->directory);
  if(info->command) free(info->command);
  if(info->stdin_pipe[0] >= 0) close(info->stdin_pipe[0]);
  if(info->stdin_pipe[1] >= 0) close(info->stdin_pipe[1]);
  if(info->stderr_pipe[0] >= 0) close(info->stderr_pipe[0]);
  if(info->stderr_pipe[1] >= 0) close(info->stderr_pipe[1]);
  free(info);
}

static int
stratcon_iep_err_handler(eventer_t e, int mask, void *closure,
                         struct timeval *now) {
  int len, newmask;
  char buff[4096];
  struct iep_daemon_info *info = (struct iep_daemon_info *)closure;

  if(mask & EVENTER_EXCEPTION) {
    int rv;
   read_error:
    kill(info->child, SIGKILL);
    if(waitpid(info->child, &rv, 0) != info->child) {
      noitL(noit_error, "Failed to reap IEP daemon\n");
      exit(-1);
    }
    noitL(noit_error, "IEP daemon is done, starting a new one\n");
    start_iep_daemon();
    eventer_remove_fd(e->fd);
    iep_daemon_info_free(info);
    return 0;
  }
  while(1) {
    len = e->opset->read(e->fd, buff, sizeof(buff)-1, &newmask, e);
    if(len == -1 && (errno == EAGAIN || errno == EINTR))
      return newmask | EVENTER_EXCEPTION;
    if(len <= 0) goto read_error;
    assert(len < sizeof(buff));
    buff[len] = '\0';
    noitL(noit_iep, "%s", buff);
  }
}

static void
start_iep_daemon() {
  eventer_t newe;
  struct iep_daemon_info *info;

  info = calloc(1, sizeof(*info));
  info->stdin_pipe[0] = info->stdin_pipe[1] = -1;
  info->stderr_pipe[0] = info->stderr_pipe[1] = -1;

  if(!noit_conf_get_string(NULL, "/stratcon/iep/start/@directory",
                           &info->directory))
    info->directory = strdup(".");
  if(!noit_conf_get_string(NULL, "/stratcon/iep/start/@command",
                           &info->command)) {
    noitL(noit_error, "No IEP start command provided.  You're on your own.\n");
    goto bail;
  }
  if(pipe(info->stdin_pipe) != 0 ||
     pipe(info->stderr_pipe) != 0) {
    noitL(noit_error, "pipe: %s\n", strerror(errno));
    goto bail;
  }
  info->child = fork();
  if(info->child == -1) {
    noitL(noit_error, "fork: %s\n", strerror(errno));
    goto bail;
  }
  if(info->child == 0) {
    char *argv[3] = { "run-iep", NULL, NULL };
    int stdout_fileno;

    argv[1] = noit_conf_config_filename();

    if(chdir(info->directory) != 0) {
      noitL(noit_error, "Starting IEP daemon, chdir failed: %s\n",
            strerror(errno));
      exit(-1);
    }

    close(info->stdin_pipe[1]);
    close(info->stderr_pipe[0]);
    dup2(info->stdin_pipe[0], 0);
    dup2(info->stderr_pipe[1], 2);
    stdout_fileno = open("/dev/null", O_WRONLY);
    dup2(stdout_fileno, 1);

    exit(execv(info->command, argv));
  }
  /* in the parent */
  close(info->stdin_pipe[0]);
  info->stdin_pipe[0] = -1;
  close(info->stderr_pipe[1]);
  info->stderr_pipe[1] = -1;
  if(eventer_set_fd_nonblocking(info->stderr_pipe[0])) {
    goto bail;
  }

  newe = eventer_alloc();
  newe->fd = info->stderr_pipe[0];
  newe->mask = EVENTER_READ | EVENTER_EXCEPTION;
  newe->callback = stratcon_iep_err_handler;
  newe->closure = info;
  eventer_add(newe);
  info = NULL;

  /* This will induce a stomp connection which will initialize esper */
  setup_iep_connection_later(1);

  return;

 bail:
  if(info) {
    iep_daemon_info_free(info);
  }
  noitL(noit_error, "Failed to start IEP daemon\n");
  exit(-1);
  return;
}

void
stratcon_iep_init() {
  noit_boolean disabled = noit_false;
  apr_initialize();
  atexit(apr_terminate);   

  if(noit_conf_get_boolean(NULL, "/stratcon/iep/@disabled", &disabled) &&
     disabled == noit_true) {
    noitL(noit_error, "IEP system is disabled!\n");
    return;
  }

  noit_iep = noit_log_stream_find("error/iep");
  if(!noit_iep) noit_iep = noit_error;

  eventer_name_callback("stratcon_iep_submitter", stratcon_iep_submitter);
  eventer_name_callback("setup_iep_connection_callback", setup_iep_connection_callback);
  pthread_key_create(&iep_connection, connection_destroy);

  /* start up a thread pool of one */
  memset(&iep_jobq, 0, sizeof(iep_jobq));
  eventer_jobq_init(&iep_jobq, "iep_submitter");
  iep_jobq.backq = eventer_default_backq();
  eventer_jobq_increase_concurrency(&iep_jobq);

  start_iep_daemon();

  /* setup our live jlog stream */
  stratcon_streamer_connection(NULL, NULL,
                               stratcon_jlog_recv_handler,
                               (void *(*)())stratcon_jlog_streamer_iep_ctx_alloc,
                               NULL,
                               jlog_streamer_ctx_free);
}
