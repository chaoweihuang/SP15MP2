#include "csiebox_client.h"

#include "csiebox_common.h"
#include "connect.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/inotify.h>
#include <hash.h>

static int parse_arg(csiebox_client* client, int argc, char** argv);
static int login(csiebox_client* client);
static int prepare_sync(csiebox_client* client);
static void sync_all(csiebox_client* client, char* longestpath, int level);
static void sync_file(csiebox_client* client, char* path);
static csiebox_protocol_status sync_meta(csiebox_client* client, char* path);
static void sync_data(csiebox_client* client, char* path);
static void add_inotify(csiebox_client* client, char* path);
static void handle_inotify_event(csiebox_client* client);
static void rm_file(csiebox_client* client, char* path, int isdir);

#define IN_FLAG (IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MODIFY)
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

int maxlevel = 0;

//read config file, and connect to server
void csiebox_client_init(
  csiebox_client** client, int argc, char** argv) {
  csiebox_client* tmp = (csiebox_client*)malloc(sizeof(csiebox_client));
  if (!tmp) {
    fprintf(stderr, "client malloc fail\n");
    return;
  }
  memset(tmp, 0, sizeof(csiebox_client));
  if (!parse_arg(tmp, argc, argv)) {
    fprintf(stderr, "Usage: %s [config file]\n", argv[0]);
    free(tmp);
    return;
  }
  int fd = client_start(tmp->arg.name, tmp->arg.server);
  if (fd < 0) {
    fprintf(stderr, "connect fail\n");
    free(tmp);
    return;
  }
  tmp->conn_fd = fd;

	int inotify_fd;
	inotify_fd = inotify_init();
	if(fd < 0){
		fprintf(stderr, "inotify fail\n");
		close(tmp->conn_fd);
		return;
	}

	tmp->inotify_fd = inotify_fd;

	memset(tmp->inotify_path, 0, 100 * PATH_MAX);
  *client = tmp;
}

//this is where client sends request, you sould write your code here
int csiebox_client_run(csiebox_client* client) {
  if (!login(client)) {
    fprintf(stderr, "login fail\n");
    return 0;
  }
  fprintf(stderr, "login success\n");
  
  if (!prepare_sync(client)){
    fprintf(stderr, "cannot sync\n");
    return 0;
  }

  fprintf(stderr, "start monitoring\n");
  while(1){
  	handle_inotify_event(client);
  }
  fprintf(stderr, "stop monitoring\n");
  return 1;
}

void csiebox_client_destroy(csiebox_client** client) {
  csiebox_client* tmp = *client;
  *client = 0;
  if (!tmp) {
    return;
  }
  close(tmp->conn_fd);
  free(tmp);
}

//read config file
static int parse_arg(csiebox_client* client, int argc, char** argv) {
  if (argc != 2) {
    return 0;
  }
  FILE* file = fopen(argv[1], "r");
  if (!file) {
    return 0;
  }
  fprintf(stderr, "reading config...\n");
  size_t keysize = 20, valsize = 20;
  char* key = (char*)malloc(sizeof(char) * keysize);
  char* val = (char*)malloc(sizeof(char) * valsize);
  ssize_t keylen, vallen;
  int accept_config_total = 5;
  int accept_config[5] = {0, 0, 0, 0, 0};
  while ((keylen = getdelim(&key, &keysize, '=', file) - 1) > 0) {
    key[keylen] = '\0';
    vallen = getline(&val, &valsize, file) - 1;
    val[vallen] = '\0';
    fprintf(stderr, "config (%d, %s)=(%d, %s)\n", keylen, key, vallen, val);
    if (strcmp("name", key) == 0) {
      if (vallen <= sizeof(client->arg.name)) {
        strncpy(client->arg.name, val, vallen);
        accept_config[0] = 1;
      }
    } else if (strcmp("server", key) == 0) {
      if (vallen <= sizeof(client->arg.server)) {
        strncpy(client->arg.server, val, vallen);
        accept_config[1] = 1;
      }
    } else if (strcmp("user", key) == 0) {
      if (vallen <= sizeof(client->arg.user)) {
        strncpy(client->arg.user, val, vallen);
        accept_config[2] = 1;
      }
    } else if (strcmp("passwd", key) == 0) {
      if (vallen <= sizeof(client->arg.passwd)) {
        strncpy(client->arg.passwd, val, vallen);
        accept_config[3] = 1;
      }
    } else if (strcmp("path", key) == 0) {
      if (vallen <= sizeof(client->arg.path)) {
        strncpy(client->arg.path, val, vallen);
        accept_config[4] = 1;
      }
    }
  }
  free(key);
  free(val);
  fclose(file);
  int i, test = 1;
  for (i = 0; i < accept_config_total; ++i) {
    test = test & accept_config[i];
  }
  if (!test) {
    fprintf(stderr, "config error\n");
    return 0;
  }
  return 1;
}

static int login(csiebox_client* client) {
  csiebox_protocol_login req;
  memset(&req, 0, sizeof(req));
  req.message.header.req.magic = CSIEBOX_PROTOCOL_MAGIC_REQ;
  req.message.header.req.op = CSIEBOX_PROTOCOL_OP_LOGIN;
  req.message.header.req.datalen = sizeof(req) - sizeof(req.message.header);
  memcpy(req.message.body.user, client->arg.user, strlen(client->arg.user));
  md5(client->arg.passwd,
      strlen(client->arg.passwd),
      req.message.body.passwd_hash);
  if (!send_message(client->conn_fd, &req, sizeof(req))) {
    fprintf(stderr, "send fail\n");
    return 0;
  }
  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  if (recv_message(client->conn_fd, &header, sizeof(header))) {
    if (header.res.magic == CSIEBOX_PROTOCOL_MAGIC_RES &&
        header.res.op == CSIEBOX_PROTOCOL_OP_LOGIN &&
        header.res.status == CSIEBOX_PROTOCOL_STATUS_OK) {
      client->client_id = header.res.client_id;
      return 1;
    } else {
      return 0;
    }
  }
  return 0;
}

static int prepare_sync(csiebox_client* client){
	char* cwd = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(cwd, 0, sizeof(cwd));
	if(getcwd(cwd, PATH_MAX) == 0){
		fprintf(stderr, "cannot get cwd\n");
		free(cwd);
		return 0;
	}

	if(chdir(client->arg.path) != 0){
		fprintf(stderr, "cannot change to client path\n");
		return 0;
	}

	maxlevel = 0;

	char* longestpath = (char*)malloc(sizeof(char) * PATH_MAX);
	sync_all(client, longestpath, 0);
	
	char* relative_longestpath = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(relative_longestpath,0,PATH_MAX);
	int i = 0;
	int rootlen = strlen(client->arg.path);
	int len = strlen(longestpath);
	for(; i < len-rootlen; i++){
		relative_longestpath[i] = longestpath[i+rootlen];
	}
	relative_longestpath[len-rootlen] = '\0';

	FILE* fp = fopen("longestPath.txt","w");
	if(!fp){
		fprintf(stderr, "file open fail\n");
		return 0;
	}
	fprintf(fp, "%s\n",relative_longestpath);
	fclose(fp);

	//fprintf(stderr, "longest path: %s\n",relative_longestpath);

	chdir(cwd);
	return 1;
}

static void sync_all(csiebox_client* client, char* longestpath, int level){	
	char* cwd = (char*)malloc(sizeof(char) * PATH_MAX);
	DIR* dir;
	struct dirent* file;
	struct stat fstat;

	memset(cwd, 0, sizeof(cwd));
	if(getcwd(cwd, PATH_MAX) == 0){
		fprintf(stderr, "cannot get cwd\n");
		free(cwd);
		return;
	}

	add_inotify(client, cwd);
	dir = opendir(".");

	while((file = readdir(dir)) != NULL){
		if(strcmp(file->d_name,".") == 0)
			continue;
		if(strcmp(file->d_name,"..") == 0)
			continue;

		lstat(file->d_name, &fstat);
		sync_file(client,file->d_name);
		if((fstat.st_mode & S_IFMT) == S_IFDIR){
			if(chdir(file->d_name) != 0){
				fprintf(stderr, "entering dir [%s] error\n", file->d_name);
				continue;
			}
			
			level++;
			if(level > maxlevel){
				maxlevel = level;
				sprintf(longestpath, "%s/%s",cwd,file->d_name);
			}

			sync_all(client, longestpath, level);

			chdir(cwd);
		}
	}

	closedir(dir);
	free(cwd);
	return;
}

static void sync_file(csiebox_client* client, char* path){
	csiebox_protocol_status status;
	status = sync_meta(client,path);
	if(status == CSIEBOX_PROTOCOL_STATUS_MORE){
		sync_data(client, path);
	}
}

static csiebox_protocol_status sync_meta(csiebox_client* client, char* path){
	csiebox_protocol_header header;
	csiebox_protocol_meta meta;
	memset(&meta,0,sizeof(meta));
	meta.message.header.req.magic = CSIEBOX_PROTOCOL_MAGIC_REQ;
	meta.message.header.req.op = CSIEBOX_PROTOCOL_OP_SYNC_META;
	meta.message.header.req.client_id = client->client_id;
	meta.message.header.req.datalen = sizeof(meta) - sizeof(header);
	
	struct stat tmpstat;
	lstat(path,&tmpstat);
	meta.message.body.stat = tmpstat;
	if((tmpstat.st_mode & S_IFMT) != S_IFDIR){
		md5_file(path,meta.message.body.hash);
	}

	char* relative_path = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(relative_path,0,PATH_MAX);
	char* cwd = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(cwd,0,PATH_MAX);
	getcwd(cwd,PATH_MAX);
	fprintf(stderr,"cwd: %s\n",cwd);
	sprintf(relative_path, "%s/%s", cwd, path);
	int i = 0;
	int rootlen = strlen(client->arg.path);
	int len = strlen(relative_path);
	for(; i < len-rootlen; i++){
		relative_path[i] = relative_path[i+rootlen];
	}
	relative_path[len-rootlen] = '\0';
	
	meta.message.body.pathlen = strlen(relative_path);

	send_message(client->conn_fd, &meta, sizeof(meta));
	fprintf(stderr,"meta sent, file path=%s\n",path);

	send_message(client->conn_fd, relative_path, strlen(relative_path));
	fprintf(stderr,"relative path sent: %s\n",relative_path);

	recv_message(client->conn_fd, &header, sizeof(header));
	fprintf(stderr,"header received\n");

	if(header.res.status == CSIEBOX_PROTOCOL_STATUS_FAIL){
		fprintf(stderr,"sync meta fail, path: %s\n",relative_path);
		return CSIEBOX_PROTOCOL_STATUS_FAIL;
	}

	return header.res.status;	
}

static void sync_data(csiebox_client* client, char* path){	
	csiebox_protocol_file file;
	memset(&file, 0, sizeof(file));
	file.message.header.req.magic = CSIEBOX_PROTOCOL_MAGIC_REQ;
	file.message.header.req.op = CSIEBOX_PROTOCOL_OP_SYNC_FILE;
	file.message.header.req.client_id = client->client_id;
	file.message.header.req.datalen = sizeof(file) - sizeof(csiebox_protocol_header);

	struct stat fstat;
	memset(&fstat,0,sizeof(fstat));
	lstat(path, &fstat);
	
	if((fstat.st_mode & S_IFMT) == S_IFDIR){
		file.message.body.datalen = 0;
		send_message(client->conn_fd, &file, sizeof(file));
	}
	else{
		FILE* fp = fopen(path, "r");
		if(!fp){
			fprintf(stderr, "cannot open file: %s\n",path);
			file.message.body.datalen = 0;
			send_message(client->conn_fd, &file, sizeof(file));
		}
		else{
			fseek(fp, 0, SEEK_END);
			file.message.body.datalen = ftell(fp);
			fprintf(stderr, "file: %s, datalen: %zd\n",path,file.message.body.datalen);
			send_message(client->conn_fd, &file, sizeof(file));
			fseek(fp, 0, SEEK_SET);
			char buf[4096];
			memset(buf,0,4096);
			size_t readlen = 0;
			while((readlen = fread(buf,sizeof(char), 4096, fp)) > 0){
				send_message(client->conn_fd, buf, readlen);
			}
			fprintf(stderr,"data sent\n",buf);
			fclose(fp);
		}
	}

	csiebox_protocol_header header;
	recv_message(client->conn_fd, &header, sizeof(header));
	fprintf(stderr, "header received, sync file end.\n");
	if(header.res.status != CSIEBOX_PROTOCOL_STATUS_OK){
		fprintf(stderr,"sync data error: %s\n",path);
	}
}

static void rm_file(csiebox_client* client, char* path, int isdir){		
	char* relative_path = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(relative_path,0,PATH_MAX);
	char* cwd = (char*)malloc(sizeof(char) * PATH_MAX);
	memset(cwd,0,PATH_MAX);
	getcwd(cwd,PATH_MAX);
	fprintf(stderr, "cwd: %s\n",cwd);
	sprintf(relative_path, "%s/%s", cwd, path);
	int i = 0;
	int rootlen = strlen(client->arg.path);
	int len = strlen(relative_path);
	for(; i < len-rootlen; i++){
		relative_path[i] = relative_path[i+rootlen];
	}
	relative_path[len-rootlen] = '\0';
	
	if(isdir){
		int i = 0;
		int wd = -1;
		for(; i < 100; i++){
			if(strncmp(path, client->inotify_path[i],strlen(client->inotify_path[i])) == 0)
				wd = i;
		}
		if(wd == -1){
			fprintf(stderr, "cannot find path\n");
			return;
		}
		inotify_rm_watch(client->inotify_fd, wd);
		memset(client->inotify_path[wd], 0, PATH_MAX);
	}

	csiebox_protocol_rm rm;
	memset(&rm, 0, sizeof(rm));
	csiebox_protocol_header header;
	memset(&header, 0, sizeof(header));
	rm.message.header.req.magic = CSIEBOX_PROTOCOL_MAGIC_REQ;
	rm.message.header.req.op = CSIEBOX_PROTOCOL_OP_RM;
	rm.message.header.req.client_id = client->client_id;
	rm.message.header.req.datalen = sizeof(rm) - sizeof(header);
	rm.message.body.pathlen = strlen(relative_path);
	send_message(client->conn_fd, &rm, sizeof(rm));
	send_message(client->conn_fd, relative_path, strlen(relative_path));
	fprintf(stderr, "relative path sent: %s\n", relative_path);
	recv_message(client->conn_fd, &header, sizeof(header));
	fprintf(stderr, "header received\n");
	if(header.res.status != CSIEBOX_PROTOCOL_STATUS_OK)
					fprintf(stderr, "rm fail: %s\n", path);

	

}

static void add_inotify(csiebox_client* client, char* path){
	int wd = inotify_add_watch(client->inotify_fd, path, IN_FLAG);

	char* relative_path = (char*)malloc(sizeof(char) * PATH_MAX);
	sprintf(relative_path, "%s", path);
	int i = 0;
	int rootlen = strlen(client->arg.path);
	int len = strlen(relative_path);
	for(; i < len-rootlen; i++){
		relative_path[i] = relative_path[i+rootlen];
	}
	relative_path[len-rootlen] = '\0';
	fprintf(stderr, "add path to wd %d, path: %s\n",wd,relative_path);	
	memcpy(client->inotify_path[wd % 100], relative_path, strlen(relative_path));	
}

static void handle_inotify_event(csiebox_client* client){
	int len = 0, i = 0;
	char buf[EVENT_BUF_LEN];
	memset(buf, 0, EVENT_BUF_LEN);

	if((len = read(client->inotify_fd, buf, EVENT_BUF_LEN)) <= 0){
		return;
	}

	i = 0;
	while (i < len){
		chdir(client->arg.path);
		struct inotify_event* event = (struct inotify_event*)&buf[i];
		char path[PATH_MAX];
		memset(path, 0, PATH_MAX);
		char wd_path[PATH_MAX];
		memset(wd_path, 0, PATH_MAX);
		
		memcpy(wd_path, client->inotify_path[event->wd % 100], PATH_MAX);
		fprintf(stderr, "wd path: %s\n", wd_path);
		
		sprintf(path, "%s/",wd_path);
		strncat(path, event->name, event->len);

		char realpath[PATH_MAX];
		memset(realpath, 0, PATH_MAX);
		sprintf(realpath, "%s%s",client->arg.path,path);
		sprintf(path, "%s", realpath);
		int rootlen = strlen(client->arg.path)+1;
		int len = strlen(realpath);
		int j = 0;
		for(; j < len - rootlen; j++){
			path[j] = path[j+rootlen];
		}
		path[len-rootlen] = '\0';

		fprintf(stderr, "inotify event wd: %d\n", event->wd);
		fprintf(stderr, "inotify event path: %s\n",path);	
		if(event->mask & IN_CREATE){
			fprintf(stderr, "type: create\n");
			sync_file(client, path);
			if(event->mask & IN_ISDIR){
				add_inotify(client, realpath);
			}
		}
		else if(event->mask & IN_ATTRIB){
			fprintf(stderr, "type: attrib\n");
			sync_meta(client, path);
		}
		else if(event->mask & IN_DELETE){
			fprintf(stderr, "type: delete\n");
			rm_file(client, path, event->mask & IN_ISDIR);
		}
		else{
			fprintf(stderr, "modify\n");
			sync_file(client, path);
		}

		i += EVENT_SIZE + event->len;
	}
	memset(buf,0,EVENT_BUF_LEN);
}
