#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H


//skynet ������ ��Ϣ
struct skynet_config {

	int thread;		//�߳���
	int harbor;		//harbor
	int profile;	
	const char * daemon;
	const char * module_path;  // ģ�� �������·�� .so�ļ�·��
	const char * bootstrap;
	
	const char * logger;	//��־����
	const char * logservice;	
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

void skynet_start(struct skynet_config * config);

#endif