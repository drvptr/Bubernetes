#ifndef APISERVER_H
#define APISERVER_H

/*Local API*/

/* opaque types */
typedef struct res res_t;
typedef struct resp resp_t;

/* Nouns */
enum { STATUS, ADDRESS, PID, REPLICAS, BINARY, MEM, CPU };

/* Verbs - Requests */
resp_t  *ResGet(res_t *in, int noun);
int      ResSet(res_t *in, int noun, resp_t *value);
int      ResWatch(res_t *in, int noun);
res_t   *ResCreate(void);
void     ResDelete(res_t *in);

/* Response Methods */

const void *RespGetValuePtr(resp_t *r);


#endif
