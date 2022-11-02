# Internals

本文档用于描述源代码的使用。



## 开发者配置

开发者使用的开发环境是Ubuntu Linux 20.04。

CCS使用10.4版本。使用10.x版本而不是11.x版本的原因是在开发阶段11.x版本尚不能很好的支持XGCONF编辑`app_ble.cfg`文件，当然用文本编辑器编辑是可以的，而且最终的项目在开发者熟悉了文件个时候，并没有把importFile的文件展开，所以也就不需要XGCONF了；在早期开发时反复用XGCONF调整设置。



使用的SDK版本（在CCS->Window->Preference->Code Composer Studio->Products看到）5.30.0.03。



## Board File

在源代码的跟目录下有一个`boards.tar.gz`压缩文件，是板级文件。

ti的官方推荐做法是把板级文件放到sdk的路径下而不是项目内，在项目开发者的Ubuntu电脑上该路径为：

```
/home/ma/ti/simplelink_cc2640r2_sdk_5_30_00_03/source/ti/boards
```

`boards.tar.gz`里包含了3个板级文件夹，实际使用的是`CC2640R2DK_5MM`。



## 源码配置

源码的编译配置都和CC2640R2的TI官方例子项目（含BLE的）没有很大区别。

实际发布固件使用的配置的RCOSC版本，即TOOLS/defines目录下激活的是后缀为`*_RCOSC.opt`的编译宏文件。

| Macro                            | Comment                                         |
| -------------------------------- | ----------------------------------------------- |
| CC2640R2DK_5MM                   | 为板级文件增加的宏，缺省开                      |
| USE_RCOSC                        | 选择RSOSC，即内部RC振荡而不是板载晶振           |
| xdc_runtime_Assert_DISABLE_ALL   | 禁止所有assert，应该使用                        |
| xdc_runtime_Log_DISABLE_ALL      | 禁止所有Log，调试时如果需要串口输出应该去掉该宏 |
| Board_EXCLUDE_NVS_INTERNAL_FLASH | 禁止在片上flash开nvs                            |
| MAX_PDU_SIZE=255和MAX_NUM_PDU=6  | 这两个宏对达到最大BLE带宽很重要                 |
| LOG_ADPCM_DATA                   | 调试用的，缺省关闭                              |
| LOG_NVS_AFTER_AUTOSTOP           | 调试用的，缺省关闭                              |
| Display_DISABLE_ALL              | 调试用的，缺省打开                              |
| LOG_BADPCM_DATA                  | 调试用的，缺省关闭                              |



因为片上内存资源太少，调试的打印不会都打开。如果要看到串口打印，需要禁止`Display_DISABLE_ALL`；如果要打印原始MIC产生的数据，可打开`LOG_ADPCM_DATA`，如果要打印蓝牙数据包，可打开`LOG_BADPCM_DATA`，但不要同时打开两者，资源不足无法工作。



## 源码说明

无法完全详细的说明所有源码细节，只能大概说一下关键设计。有任何问题联系原始开发者（马天夫，matianfu@gingerologist.com，微信：unwiredgran）

核心文件：

| 文件名              | 功能     |
| ------------------- | -------- |
| button.c            | 按键任务 |
| audio.c             | 录音任务 |
| simple_peripheral.c | 蓝牙任务 |



### button.c

按键任务里使用`btn[6]`记录每次sampling的按键状态，但最后一个数组元素不使用，类似null-terminated string。该数组的数据格式是counting连续按下或者抬起的采样次数，正数为按下，负数为抬起，初值都是0。`btn[4]`是当前累计值。如果发生按下和抬起的切换，所有值向前shift一格。在这种设计下，例如：

- btn[4] = 200ms当作长按的判断。

其它按键判断方法见代码内的宏定义。注意首次开机时的判断方法和其它情况还有不一样，首次开机需要额外判断开机动作失败，要尽可能早的判断出失败（即判定为误按）然后关机。



### audio.c

audio.c的工作流程是：

1. 从麦克风采样，使用多个buffer；
2. 存入flash；
3. 发送给ble任务，该任务只用一个buffer，是所谓的pull方式的实现；ble任务是consumer；



### flash存储格式

flash以最后两个sector（4k）实现了一个单调递增的counter，用于记录全局的sector index；

最后16个sector不用于存储音频（含单调counter）；

每个4KB的sector，4096字节里，有4000字节是ADPCM格式的音频文件，其余96字节是一个位于头部的结构体，包含24个uint32_t，其中有21个历史记录，当前记录，和当前adpcmState。具体可以参见ctx_t结构体定义。



flash的写入逻辑如下：

1. 先擦除4k
2. 写入头
3. 依次写入adpcm数据
4. 全部写入完成后更新monotone counter，递增1

monotone counter是记录当前位置的。系统启动时读入该值。该值使用ring buffer逻辑。超过（总数-16）持续增加，但计算物理位置时要mod一下。



## simple_peripheral.c

和ti的例子比，仅最大可能删除不必要的代码以节约flash和ram。

在发送上，ti没有填充底层buffer的好办法，只能尽可能填充然后busy polling。所以在打开log时会看到大量的fail，其实只是填充buffer fail，不是真的失败。但在信号较好的时候，fail较少，速度快；如果信号差，fail会很多，传输慢。



