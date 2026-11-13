# 需求
这个工程是一个基于moq的播放器，用于播放Media over Quic协议的视频流。

我们的通信协议采用Media over Quic协议, 基于开源：https://github.com/runner365/moqlivecast.git

其中采用其: moq transport client的c语言sdk客户端，用于与moq服务器进行media pull的操作，进行拉流；


我们将https://github.com/runner365/moqlivecast.git这个开源作为我们的submodule, 放入upstream目录下。

然后开发moq_player.cpp, 用于实现播放器的功能。


分3部分：
* Media over Quic协议的开源应用，moq transport client的c语言sdk客户端，用于与moq服务器进行media pull的操作，进行拉流。
* 拉流成功后，对其H264+AAC进行解码，解码采用：3rdparty/ffmpeg.tar.gz中的ffmpeg库，进行解码。
* 解码后的视频流和音频流，通过SDL2库进行渲染和播放，3rdparty/SDL3-3.4.20.tar.gz

也就是整个工程，需要写一个cmakelist.txt文件，用于配置我们的工程, 加入moq transport client的c语言sdk客户端, 以及ffmpeg库, SDL2库。

开始第 3 步 SDL3 渲染播放；加一个开关：av_sync，是否音视频同步；默认去使能；
使能后，加入队列，队列按照pts的时间戳进行排序，av_sync=2000，意味这个队列长度为2000ms的长度。
默认不使能，直接放入队列后，队列按照pts的时间戳进行排序，sdk模块直接定时读取队列对应的pts的avframe进行播放；
