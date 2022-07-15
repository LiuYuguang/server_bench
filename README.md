# 服务端压力测试工具  
1. 对wrk的模仿，主要解决wrk不支持请求多个host:port。内网下压测，单机对单机的连接，常常会触及连接数上限（socket五元组）。  
2. epoll+reactor模型+多线程  
