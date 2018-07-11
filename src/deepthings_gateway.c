#include "deepthings_gateway.h"
#include "config.h"
#include "ftp.h"
#include "inference_engine_helper.h"
#include "frame_partitioner.h"
#include "reuse_data_serialization.h"
#if DEBUG_TIMING
static double start_time;
static double acc_time[CLI_NUM];
static uint32_t acc_frames[CLI_NUM];
#endif
#if DEBUG_COMMU_SIZE
static double commu_size;
#endif

cnn_model* deepthings_gateway_init(){
   init_gateway();
   cnn_model* model = load_cnn_model((char*)"models/yolo.cfg", (char*)"models/yolo.weights");
   model->ftp_para = preform_ftp(PARTITIONS_H, PARTITIONS_W, FUSED_LAYERS, model->net_para);
#if DATA_REUSE
   model->ftp_para_reuse = preform_ftp_reuse(model->net_para, model->ftp_para);
#endif
   return model;
}


#if DATA_REUSE
void notify_coverage(cnn_model* model, blob* task_input_blob, uint32_t cli_id){
   char data[20]="empty";
   blob* temp = new_blob_and_copy_data(cli_id, 20, (uint8_t*)data);
   copy_blob_meta(temp, task_input_blob);
   service_conn* conn;
   conn = connect_service(TCP, get_client_addr(cli_id), WORK_STEAL_PORT);
   send_request("update_coverage", 20, conn);
   send_data(temp, conn);
   free_blob(temp);
   close_service_connection(conn);
}
#endif


void* deepthings_result_gateway(void* srv_conn, void* model){
   printf("result_gateway ... ... \n");
   service_conn *conn = (service_conn *)srv_conn;
   int32_t cli_id;
   int32_t frame_seq;
#if DEBUG_FLAG
   char ip_addr[ADDRSTRLEN];
   int32_t processing_cli_id;
   inet_ntop(conn->serv_addr_ptr->sin_family, &(conn->serv_addr_ptr->sin_addr), ip_addr, ADDRSTRLEN);
   processing_cli_id = get_client_id(ip_addr);
#if DEBUG_TIMING
   double total_time;
   uint32_t total_frames;
   double now;
   uint32_t i;
#endif
   if(processing_cli_id < 0)
      printf("Client IP address unknown ... ...\n");
#endif
   blob* temp = recv_data(conn);
   cli_id = get_blob_cli_id(temp);
   frame_seq = get_blob_frame_seq(temp);
#if DEBUG_COMMU_SIZE
   commu_size = commu_size + temp->size;
#endif
#if DEBUG_FLAG
   printf("Result from %d: %s is for client %d, total number recved is %d\n", processing_cli_id, ip_addr, cli_id, results_counter[cli_id]);
#endif
   enqueue(results_pool[cli_id], temp);
   free_blob(temp);
   results_counter[cli_id]++;
   if(results_counter[cli_id] == ((cnn_model*)model)->ftp_para->partitions){
      temp = new_empty_blob(cli_id);
#if DEBUG_FLAG
      printf("Results for client %d are all collected in deepthings_result_gateway, update ready_pool\n", cli_id);
#endif
#if DEBUG_TIMING
      printf("Client %d, frame sequence number %d, all partitions are merged in deepthings_merge_result_thread\n", cli_id, frame_seq);
      now = sys_now_in_sec();
      /*Total latency*/
      acc_time[cli_id] = now - start_time;
      acc_frames[cli_id] = frame_seq + 1;
      total_time = 0;
      total_frames = 0;
      for(i = 0; i < CLI_NUM; i ++){
         if(acc_frames[i] > 0)
             printf("Avg latency for Client %d is: %f\n", i, acc_time[i]/acc_frames[i]);
         total_time = total_time + acc_time[i];
         total_frames = total_frames + acc_frames[i];
      }
      printf("Avg latency for all clients %f\n", total_time/total_frames);
#endif
#if DEBUG_COMMU_SIZE
      printf("Communication size at gateway is: %f\n", ((double)commu_size)/(1024.0*1024.0*FRAME_NUM));
#endif
      enqueue(ready_pool, temp);
      free_blob(temp);
      results_counter[cli_id] = 0;
   }

   return NULL;
}

void deepthings_collect_result_thread(void *arg){
   const char* request_types[]={"result_gateway"};
   void* (*handlers[])(void*, void*) = {deepthings_result_gateway};
   int result_service = service_init(RESULT_COLLECT_PORT, TCP);
   start_service(result_service, TCP, request_types, 1, handlers, arg);
   close_service(result_service);
}

void deepthings_merge_result_thread(void *arg){
   cnn_model* model = (cnn_model*)arg;
#ifdef NNPACK
   nnp_initialize();
   model->net->threadpool = pthreadpool_create(THREAD_NUM);
#endif
   blob* temp;
   int32_t cli_id;
   int32_t frame_seq;
   while(1){
      temp = dequeue_and_merge(model);
      cli_id = get_blob_cli_id(temp);
      frame_seq = get_blob_frame_seq(temp);
#if DEBUG_FLAG
      printf("Client %d, frame sequence number %d, all partitions are merged in deepthings_merge_result_thread\n", cli_id, frame_seq);
#endif
      float* fused_output = (float*)(temp->data);
      image_holder img = load_image_as_model_input(model, get_blob_frame_seq(temp));
      set_model_input(model, fused_output);
      forward_all(model, model->ftp_para->fused_layers);   
      draw_object_boxes(model, get_blob_frame_seq(temp));
      free_image_holder(model, img);
      free_blob(temp);
#if DEBUG_FLAG
      printf("Client %d, frame sequence number %d, finish processing\n", cli_id, frame_seq);
#endif
   }
#ifdef NNPACK
   pthreadpool_destroy(model->net->threadpool);
   nnp_deinitialize();
#endif
}


#if DATA_REUSE
static overlapped_tile_data* overlapped_data_pool[CLI_NUM][PARTITIONS_MAX];
/*
static bool partition_coverage[CLI_NUM][PARTITIONS_MAX];
*/
void* recv_reuse_data_from_edge(void* srv_conn, void* model){
   printf("collecting_reuse_data ... ... \n");
   service_conn *conn = (service_conn *)srv_conn;
   cnn_model* gateway_model = (cnn_model*)model;

   int32_t cli_id;
   int32_t task_id;

   char ip_addr[ADDRSTRLEN];
   int32_t processing_cli_id;
   inet_ntop(conn->serv_addr_ptr->sin_family, &(conn->serv_addr_ptr->sin_addr), ip_addr, ADDRSTRLEN);
   processing_cli_id = get_client_id(ip_addr);
   if(processing_cli_id < 0)
      printf("Client IP address unknown ... ...\n");

   blob* temp = recv_data(conn);
   cli_id = get_blob_cli_id(temp);
   task_id = get_blob_task_id(temp);
#if DEBUG_COMMU_SIZE
   commu_size = commu_size + temp->size;
#endif

#if DEBUG_DEEP_GATEWAY
   printf("Overlapped data for client %d, task %d is collected from %d: %s, size is %d\n", cli_id, task_id, processing_cli_id, ip_addr, temp->size);
#endif
   if(overlapped_data_pool[cli_id][task_id] != NULL)
      free_self_overlapped_tile_data(gateway_model,  overlapped_data_pool[cli_id][task_id]);
   overlapped_data_pool[cli_id][task_id] = self_reuse_data_deserialization(gateway_model, task_id, (float*)temp->data, get_blob_frame_seq(temp));

   if(processing_cli_id != cli_id) notify_coverage(gateway_model, temp, cli_id);
   free_blob(temp);

   return NULL;
}

void* send_reuse_data_to_edge(void* srv_conn, void* model){
   printf("handing_out_reuse_data ... ... \n");
   service_conn *conn = (service_conn *)srv_conn;
   cnn_model* gateway_model = (cnn_model*)model;

   int32_t cli_id;
   int32_t task_id;
   uint32_t frame_num;
   blob* temp = recv_data(conn);
   cli_id = get_blob_cli_id(temp);
   task_id = get_blob_task_id(temp);
   frame_num = get_blob_frame_seq(temp);
   free_blob(temp);

#if DEBUG_DEEP_GATEWAY
   char ip_addr[ADDRSTRLEN];
   int32_t processing_cli_id;
   inet_ntop(conn->serv_addr_ptr->sin_family, &(conn->serv_addr_ptr->sin_addr), ip_addr, ADDRSTRLEN);
   processing_cli_id = get_client_id(ip_addr);
   if(processing_cli_id < 0)
      printf("Client IP address unknown ... ...\n");
#endif

   blob* reuse_info_blob = recv_data(conn);
   bool* reuse_data_is_required = (bool*)(reuse_info_blob->data);

#if DEBUG_DEEP_GATEWAY
   printf("Overlapped data for client %d, task %d is required by %d: %s is \n", cli_id, task_id, processing_cli_id, ip_addr);
   print_reuse_data_is_required(reuse_data_is_required);
#endif
   uint32_t position;
   int32_t* adjacent_id = get_adjacent_task_id_list(gateway_model, task_id);

   for(position = 0; position < 4; position++){
      if(adjacent_id[position]==-1) continue;
      if(reuse_data_is_required[position]){
#if DEBUG_DEEP_GATEWAY
         printf("place_self_deserialized_data for client %d, task %d, the adjacent task is %d\n", cli_id, task_id, adjacent_id[position]);
#endif
         place_self_deserialized_data(gateway_model, adjacent_id[position], overlapped_data_pool[cli_id][adjacent_id[position]]);
      }
   }
   free(adjacent_id);
   temp = adjacent_reuse_data_serialization(gateway_model, task_id, frame_num, reuse_data_is_required);
   free_blob(reuse_info_blob);
   send_data(temp, conn);
#if DEBUG_COMMU_SIZE
   commu_size = commu_size + temp->size;
#endif
   free_blob(temp);

   return NULL;
}

#endif

void deepthings_work_stealing_thread(void *arg){
#if DATA_REUSE
   const char* request_types[]={"register_gateway", "cancel_gateway", "steal_gateway", "reuse_data", "request_reuse_data"};
   void* (*handlers[])(void*, void*) = {register_gateway, cancel_gateway, steal_gateway, recv_reuse_data_from_edge, send_reuse_data_to_edge};
#else
   const char* request_types[]={"register_gateway", "cancel_gateway", "steal_gateway"};
   void* (*handlers[])(void*, void*) = {register_gateway, cancel_gateway, steal_gateway};
#endif

   int wst_service = service_init(WORK_STEAL_PORT, TCP);
#if DATA_REUSE
   start_service(wst_service, TCP, request_types, 5, handlers, arg);
#else
   start_service(wst_service, TCP, request_types, 3, handlers, arg);
#endif
   close_service(wst_service);
}


void deepthings_gateway(){
   cnn_model* model = deepthings_gateway_init();
   sys_thread_t t3 = sys_thread_new("deepthings_work_stealing_thread", deepthings_work_stealing_thread, model, 0, 0);
   sys_thread_t t1 = sys_thread_new("deepthings_collect_result_thread", deepthings_collect_result_thread, model, 0, 0);
   sys_thread_t t2 = sys_thread_new("deepthings_merge_result_thread", deepthings_merge_result_thread, model, 0, 0);
   exec_barrier(START_CTRL, TCP);
#if DEBUG_TIMING
   start_time = sys_now_in_sec();
#endif
   sys_thread_join(t1);
   sys_thread_join(t2);
   sys_thread_join(t3);
}



