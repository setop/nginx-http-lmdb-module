#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <lmdb.h>

#ifdef DEBUG
 #define D if(1) 
#else
 #define D if(0) 
#endif


#define CONTENT_TYPE "application/octet-stream"

static char* ngx_http_lmdb(ngx_conf_t * cf, ngx_command_t * cmd, void * conf);

typedef struct {
    ngx_str_t dbfilepath;
    ngx_str_t contentype;
} ngx_http_lmdb_loc_conf_t;

static ngx_command_t ngx_http_lmdb_commands[] = {
    {   // module command is named "lmdb":
        ngx_string("lmdb"),
        // The directive may be specified in the location-level of your nginx-config.
        // The directive does not take any arguments (NGX_CONF_NOARGS)
        NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
        // A pointer to our handler-function.
        ngx_http_lmdb,
        // We're not using these two, they're related to the configuration structures.
        0, 0,
        // A pointer to a post-processing handler.
        NULL 
    },
    {   // property: "lmdb_dbfilepath":
        ngx_string("lmdb_dbfilepath"),
        // can be specified in the location level of the config, not main nor server
        // the directive takes 1 argument
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        // A builtin function for setting string variables
        ngx_conf_set_str_slot,
        // We tell nginx how we're storing the config.
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_lmdb_loc_conf_t, dbfilepath),
        NULL
    },
    {   // property: "lmdb_content_type":
        ngx_string("lmdb_content_type"),
        // can be specified in the location level of the config, not main nor server
        // the directive takes 1 argument
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        // A builtin function for setting string variables
        ngx_conf_set_str_slot,
        // We tell nginx how we're storing the config.
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_lmdb_loc_conf_t, contentype),
        NULL
    },
    ngx_null_command
};

static void* ngx_http_lmdb_create_loc_conf(ngx_conf_t *cf) {
    D fprintf(stderr, "enter ngx_http_lmdb_create_loc_conf\n");
    ngx_http_lmdb_loc_conf_t  *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_lmdb_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    return conf;
}

static char* ngx_http_lmdb_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    D fprintf(stderr, "enter ngx_http_lmdb_merge_loc_conf\n");
    ngx_http_lmdb_loc_conf_t *prev = parent;
    ngx_http_lmdb_loc_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->dbfilepath, prev->dbfilepath, "");
    ngx_conf_merge_str_value(conf->contentype, prev->contentype, CONTENT_TYPE);
    D fprintf(stderr, "content type is set to %s\n", conf->contentype.data);
    return NGX_CONF_OK;
}

static ngx_http_module_t ngx_http_lmdb_module_ctx = {
    NULL,                           // preconfiguration
    NULL,                           // postconfiguration
    NULL,                           // create main configuration
    NULL,                           // init main configuration
    NULL,                           // create server configuration
    NULL,                           // merge server configuration
    ngx_http_lmdb_create_loc_conf,  // create location configuration
    ngx_http_lmdb_merge_loc_conf    // merge location configuration
};

ngx_module_t ngx_http_lmdb_module = {
    NGX_MODULE_V1,
    &ngx_http_lmdb_module_ctx,   // module context
    ngx_http_lmdb_commands,      // module directives
    NGX_HTTP_MODULE,            // module type
    NULL,                       // init master
    NULL,                       // init module
    NULL,                       // init process
    NULL,                       // init thread
    NULL,                       // exit thread
    NULL,                       // exit process
    NULL,                       // exit master
    NGX_MODULE_V1_PADDING
};

/*
 * magic appends here : handle HTTP request
 */
static ngx_int_t ngx_http_lmdb_handler(ngx_http_request_t * r) {
    D fprintf(stderr, "request_line: %s\n" , r->request_line.data );
    D fprintf(stderr, "uri         : %s\n" , r->uri.data           );
    D fprintf(stderr, "args        : %s\n" , r->args.data          );
    D fprintf(stderr, "exten       : %s\n" , r->exten.data         );
    D fprintf(stderr, "unparsed_uri: %s\n" , r->unparsed_uri.data );

    ngx_int_t   rc;

    ngx_http_lmdb_loc_conf_t  *cglcf;
    cglcf = ngx_http_get_module_loc_conf(r, ngx_http_lmdb_module);

    // we response to 'GET' requests only
    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    // discard request body, since we don't need it here
    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    // 0-open env
    // IMPROVE should be done only once per worker and stored for the location
    MDB_env *env;
    MDB_dbi dbi;
    MDB_val key, data;
    MDB_txn *txn;

    mdb_env_create(&env);
    D fprintf( stderr, "try to open db: %s\n", (const char*)cglcf->dbfilepath.data);
    mdb_env_open(env, (const char*)cglcf->dbfilepath.data, MDB_RDONLY /*|MDB_FIXEDMAP |MDB_NOSYNC*/, 0664);

    // 1- parse uri : get "key" from "/potentially/long/path/to/key"
    // surprisingly uri is populated as "/path/of/uri HTTP/1.1\n<first word of next header>", so has to be cleaned
    char * uridata=(char *)r->uri.data;
    D fprintf( stderr, "uri is #%s#\n", uridata);
    char * keystrto=strchr(uridata,' ');
    int uri_size = keystrto-uridata;
    if(uri_size>=128) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    char keyarr[128]; // IMPROVE should not be static, use nginx allocator and the right pool
    strncpy(keyarr,uridata,uri_size);
    keyarr[uri_size]=0;
    D fprintf( stderr, "uri is now #%s#\n", keyarr);
    char * keystr = strrchr(keyarr, '/')+1;
    D fprintf( stderr, "%p %p %d\n", keyarr, keystr, keystr-keyarr);
    strncpy(keyarr,keystr,keystr-keyarr);
    keyarr[keystr-keyarr]=0;
    key.mv_size = strlen(keyarr);
    key.mv_data = keyarr;
    D fprintf( stderr, "try to read #%s# of size %d\n", (char *)key.mv_data, key.mv_size);

    // 2- begin readonly tx, get data
    mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
    mdb_open(txn, NULL, 0, &dbi);
    rc = mdb_get(txn, dbi, &key, &data);
    if (rc==MDB_NOTFOUND) {
    	    return NGX_HTTP_NOT_FOUND;
    } else if (rc!=0) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    D fprintf( stderr, "value size %d\n", data.mv_size);
    D fprintf( stderr, "value is #%s#\n", (char *)data.mv_data);

    // 3- copy data to output buffer
    ngx_chain_t out;
    ngx_buf_t *b;
    b= ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    unsigned char *d = ngx_pcalloc(r->pool, data.mv_size);
    if (b == NULL || d == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(d, data.mv_data, data.mv_size); // IMPROVE : try to avoid memecopy

    b->pos = d;
    b->last = d + data.mv_size;
    b->memory = 1;
    b->mmap = 1;
    b->last_buf = 1;

    // 4- close, tx, env
    mdb_txn_abort(txn);
    mdb_env_close(env);

    // set the 'Content-type' header
    r->headers_out.content_type_len = cglcf->contentype.len;
    r->headers_out.content_type.len = cglcf->contentype.len;
    r->headers_out.content_type.data = (u_char * ) cglcf->contentype.data;
    // set the status line
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = data.mv_size;

    // send the headers of your response
    rc = ngx_http_send_header(r);

    // We've added the NGX_HTTP_HEAD check here, because we're unable to set content length before
    // we've actually calculated it (which is done by generating the image).
    // This is a waste of resources, and is why caching solutions exist.
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only || r->method == NGX_HTTP_HEAD) {
        return rc;
    }

    out.buf = b;
    out.next = NULL;

    // send the buffer chain of your response
    return ngx_http_output_filter(r, &out);
}

static char* ngx_http_lmdb(ngx_conf_t * cf, ngx_command_t * cmd, void * conf)
{
    ngx_http_core_loc_conf_t * clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_lmdb_handler; // handler to process the 'lmdb' directive

    return NGX_CONF_OK;
}
