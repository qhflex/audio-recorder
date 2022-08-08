# Audio Format, Storage, and BLE Interface

## Document Version

author: matianfu (at) gingerologist.com

2022-08-07, initial version, draft



## Scope and Audience

本文档描述BLE Audio Recorder设备固件使用的音频格式，存储方式，和蓝牙低功耗访问接口，包括输出的数据格式和客户端指令。

本文档不包含固件内部设计描述、调试和测试方式（由其它文档描述）。

所有相关软硬件件开发人员和测试人员都应该阅读本文档。



## 音频格式

MEMS Microphone产生的原始数据格式为signed 16bit PCM（Pulse Code Modulation）格式，APP或其它客户端软件解码后也还原成该格式播放；内部存储和输出均使用如下音频格式：

- 存储和接口传输均使用ADPCM（Adaptive Differential PCM）格式，后述；
- 设备只有一个麦克风，音频数据配置为单声道模式（而不是立体声模式但LR使用同样数据）；
- 固件内部固定使用16000采样频率，该采样率下单独录音存储和单独蓝牙读取均无问题；
  - 固件目前不排除同时录音和蓝牙读取录音的使用方式，但该使用方式不在强制需求范围内，如果有显著性能影响，会禁用该工作方式；APP开发者不应假定同时工作模式是可用的；

### ADPCM说明

ADPCM使用一个近似值查表的方法有损压缩语音数据，压缩比固定为4:1，即每个16bit PCM Sample压缩成4bit；一段（chunk）PCM Sample压缩成ADPCM后除了语音数据还额外需要3个byte存储编解码器的工作状态（ADPCM state），该状态包含一个16bit PCM Sample，代码里一般命名为`prevsample`，类型为`int16_t`（注意是有符号的），和一个`char`类型的index，该index的实际取值范围仅为0~15，所以`signed char`或`unsigned char`均可。



ADPCM使用的近似表有很多种，因此ADPCM实际上有很多种；固件采用的格式是IMA ADPCM，该格式无需硬浮点和硬乘法器，仅需处理器支持bit shift即可。本项目固件使用[Microchip提供源码的ADPCM编解码器实现](https://ww1.microchip.com/downloads/en/AppNotes/00643b.pdf)。



如果在Windows平台上使用设备产生的ADPCM语音数据，注意Microsoft提供的ADPCM Codec有两种，XAudio2里缺省使用的是Microsoft ADPCM格式，不是Microsoft IMA ADPCM格式，如果开发者使用Microsoft的Codec播放要找到办法配置成后面一种；如何配置需自行搜索，固件开发者不熟悉Windows/.net平台开发，无法提供支持。



流行的WAV文件格式支持多种ADPCM格式，包括IMA ADPCM格式，但文件头需使用Microsoft定义的扩展（extension）格式；如果需要把设备产生的ADPCM数据直接拼成WAV文件，使用标准的音频播放器播放（不限于Windows平台），可以采取这种方式；但同样，固件开发者无法提供相关技术支持，有该需求的开发者请自行搜索Microsoft提供的文档。



与MP3等音频格式不同，ADPCM没有统一的帧（Frame）的概念或约定俗成的容器格式。如果一段完整音频的数据可保证正确，帧不是必要的，甚至保存编解码器状态也不必要，因为肯定有预定义的初始化状态，比如IMA ADPCM的编解码器状态，sample和index均初始化为0。



实际使用中应用会根据需求和实现上的限制自定义类似帧的『段』（Chunk），每个Chunk记录一份Chunk开始时编解码器状态，这样在回放时允许从中间的Chunk直接开始播放，否则的话只能从头开始解码以获取当前段的编解码器状态；另外在音频数据因各种原因出错时，每段存储解码器状态可将错误影响范围限于一个Chunk内，不产生全局的影响。

- Microsoft的WAV格式里，允许文件自定义Chunk的大小



## 存储

硬件包含一个SPI NOR Flash，最小擦写单元是Sector（4096 Bytes），固件也使用Sector作为读取时的寻址方式。实际存储，每个Sector使用4000字节存储ADPCM数据，其余96字节存储固件内部使用数据，根据前一节所描述的ADPCM音频格式，每个Sample 4bit，刚好对应8000个Sample，是0.5秒的音频数据。



Flash的实际容量为128Mbits（16M Bytes），但应用开发者对容量无需有假设；从应用的角度看，设备存储视为一个单调增长的线性地址空间，从0开始。每次开始录音时就写入当前可用的Sector，写满后开始写下一个，以此类推；当所有的数据Sector都写满后，内部会再次从0开始，但是应用获得的Sector地址并不回到0，而是继续增长。



用实际的数据给例子，实际上Flash有32768个Sector，固件内部保留了最后的16个Sector做特殊用途，剩余32752个作为存储音频数据使用。录音时从Sector 0开始写到32751时都不会发生覆盖，但写到32752时，实际上写的是物理上地址为0的Sector。在绝大多数情况下应用开发者不需要关心固件内部如何翻译逻辑地址到物理地址，除非应用程序读取的Sector地址数据已经被覆盖，此时固件不会返回错误，而是会自动调整到第一个可读的Sector地址。



如下图所示，当覆盖发生时，录音位置的Sector地址A可以大于32752，包括大于该值的几倍，即发生过多轮覆盖。此时地址B指向的Sector是最小可读的，即满足B + 32752 - 1 = A，此时如果应用读取地址C（< B）的数据，固件目前的设计不会汇报错误，而是从B开始返回数据，且数据上会标注实际地址。这样应用知道读到了已经被覆盖的地址。这个设计对于每次只想从头开始获取所有录音数据的情况时方便的，应用只需提供0作为目标读取地址即可，无需自己计算那些地址已经被覆盖。

```
0 1 2 ....                                                                                                    
| | | | | | | | | | | | | | | | | | | | | | | | | | | | | | |.....| | | | | | | | | | | | | | | | | | | | | | |....
                                      ^                           ^                                           ^
                                      |                           |                                           |
                                      |                           |                                           A 当前写入地址
                                      |                           B 当前尚未被覆盖的最小地址
                                      C 客户端想读的地址
```



## BLE接口设计

### Service and Characteristic

Service和Characteristic使用的模板是：`7c95XXXX-6d0c-436f-81c8-3fd7e3db0610`，其中XXXX是短ID代入的值；完整定义如下：

- 全局仅定义一个服务，短ID是`9500`，全长UUID是`7c959500-6d0c-436f-81c8-3fd7e3db0610`；

- 上述服务仅包含一个Characteristic，短ID是`9501`，全长UUID是`7c959501-6d0c-436f-81c8-3fd7e3db0610`；
  - 该Characteristic提供`write`和`notification`能力，其中`write`当且仅当打开`notification`时有效，否则客户端写入的值都被忽略。



### Notification Data

固件提供两种Notification数据：ADPCM数据和状态（Status），任何命令写入都会返回（Status），读命令会产生持续的ADPCM数据返回。两种返回都是固定长度且大小不同，所以无需额外定义数据帧，标注长度和类型来区分。

> 两种数据格式都需要使用大数据包。所以应用程序必须先协商MTU，原则上应保证MTU协商到251字节，这是固件内部设置，也是CC2640R2芯片能支持到的最大值，使用nRF Connect应用调试可以看到该目标是可以达到的，固件开发者使用Nokia 7 Plus（很古老的手机）和Motorola Edge X30均可达到该目标。



#### Endianness

注意大于1字节宽度的所有数据类型都使用Little Endian格式存储，这是BLE的编码习惯，也是绝大多数现行MCU的缺省配置，包括ARM Cortex M。



但是在文档书写上仍然使用常用习惯，例如`uint32_t`格式的1写为`0x00000001`，但是在数据传输上，用Byte Array解释时，看到的是`01 00 00 00`，包括使用nRF Connect或者Lightblue调试时。本文档约定在使用C格式时均以`0x`作为前缀表达数据值，在使用Little Endian表达BLE命令时不使用`0x`前缀且每字节分开。就象本段落里给出的例子一样。



注意即使在使用Little Endian格式表达时，包括在nRF Connect或者LightBlue软件界面上看到的Byte Array格式，`01`里的两个字符仍然是4个most significant bits在前面（即0），4个least significant bits在后面（即1），换句话说，Endianness没有体现在bit order上，仅仅体现在byte order上。

> 实际上Endianness对bit order是有影响的，在C语言里如果用bit定义结构体内的整数位宽就会发现Endianness也应用在bits上，但是这里讨论的实际上是一个显示上的写法（notation）问题，并不是编译器怎么处理bits的问题。所以bit order和endianness的关系和这一段说的事情是无关的。



#### Status Data

Status数据的C语言格式如下，`recordings`是包含21个元素的数组，含义后述；该数据结构的大小为108字节，包含27个`uint32_t`类型的数据。

```C
#define NUM_RECS				21

typedef struct __attribute__ ((__packed__)) StatusPacket
{
  uint32_t flags; 
  uint32_t recordings[NUM_RECS];
  uint32_t recStart;
  uint32_t recPos;
  uint32_t readStart;
  uint32_t readPosMajor;
  uint32_t readPosMinor;
} StatusPacket_t;
```



`flags`目前仅使用最低的两位；最低位表示`recording`，设为1表示当前正在录音；次低位表示`reading`，设为1表示当前正在读取录音数据。因为设备的设置是只接受一个BLE连接，实际上客户端是知道`reading`状态的，在Status里输出该信息主要是方便调试。



在设备内部，录音和读取录音数据是两个相对独立的部分。rec前缀的成员表示的都是和录音状态有关的数据，read前缀的成员表示的都是和读取录音状态有关的数据。

- 录音
  - 如果当前正在录音，即recording flag为1，`recStart`是当前这段录音的起始sector地址；`recPos`是当前正在写入录音的sector地址；
  - 如果当前不在录音，即recording flag为0，`recStart`和`recPos`是相等的，指向下一次录音开始时的为sector地址；
  - 和数组类似，`recPos`这个值即可以解释成当前正在写入的index，也可以解释成总计写入过的sector的数量；但因为是循环写入设计，写入过的地址如果太早可能已经被覆盖了，无法再读回来；如果尚未发生覆盖，从0到`recPos-1`的数据都可以读出；
- 读取
  - `readStart`是当前读取请求给的原始参数，该参数不会调整，即使它小于当前尚未被覆盖的最小的Sector地址；
  - `readPosMajor`是当前读取的Sector地址；
  - `readPosMinor`是当前读取的Sector里的更小的读取单元的index；

一个Sector有4000个字节，每个ADPCM数据包只包含160个ADPCM数据，所以readPosMinor的实际取值范围是0-24。



对于应用开发者来说，应该没有业务需求会解释当前读取行为的状态参数，请求是应用自己发出的，如果notification关闭或者蓝牙断开，读取操作也就自动结束，不会影响到下一次连接。这里提供的read前缀的状态参数可以看作仅固件开发者调试使用。



对录音来说，手机与设备建立蓝牙连接时有可能已经开始了录音，应用可以根据recording flag判断状态，也可以根据`recStart`和`recPos`判断当前录音已经进行了多长时间（一个sector表示0.5s）；`recStart`还和下一节阐述的录音分段记录有关。



#### Recordings

Status Data里的`recordings`数组，加上`recStart`正好定义了最近21条历史录音记录的分段起始和结束的位置。



`recordings`数组的初始值全部设定为`0xffffffff`，表示这一条记录没有值。`recStart`在初始化的时候设置为0。在一条录音记录都没有的时候，`recordings`和`recStart`如下表所示：

| name                      | value      |
| ------------------------- | ---------- |
| recordings[0]             | 0xffffffff |
| recordings[1]             | 0xffffffff |
| ...                       | ...        |
| recordings[19]            | 0xffffffff |
| recordings[20]            | 0xffffffff |
| recordings[21] (recStart) | 0x00000000 |



如果第一段录音写满了5个Sector后录音结束，`recordings`和`recStart`会变成如下状态，表示目前有一段录音，从地址0（包含）开始到地址5（不包含）结束，依次类推。
| index                         | value      |
| ----------------------------- | ---------- |
| 0                             | 0xffffffff |
| 1                             | 0xffffffff |
| ...                           | ...        |
| 19                            | 0xffffffff |
| 20 (最后一个`recordings`元素) | 0x00000000 |
| 21 (实际上是`recStart`)       | 0x00000005 |



在固件的角度看，每次录音结束时这个列表里记录的数据『上移』一格，index为0的数据被抛弃。如果有`0xffffffff`，应用程序可以抛弃前面的`0xffffffff`，剩下部分是当前记录的所有录音分段的位置。

但是有几点要注意：

1) 录音分段如果遇到被覆盖的情况，固件目前不做处理`recordings`里的数据，即不保证`recordings`里记录的位置的数据都还可用；应用程序可以根据读取ADPCM数据的实际结果自行处理；
2) 一段录音的开始部分已经被覆盖但结束部分尚未被覆盖是完全可能遇到的，应用程序需要自己检查这种情况并考虑如何应对；极端的情况是持续的超长时间录音，比如连续录音超过几十分钟，会导致`recording`记录只有最后一条是可用的；如果录音尚未停止那就一条可用的都没有了，这个情况要求应用程序必须能处理被部分覆盖的语音段落，而不是简单抛弃；
3) 如果每次录音的时间很短，比如只有几秒钟，这个最多存储21条记录的表很快就会被填满；被填满不意味着前面的录音数据丢失，应用程序仍可以用0作为读取命令的起始参数把所有存储的语音数据都读回来，但前面的数据其分段信息已经丢失；

> 如果要克服3所述的问题，当前的数据结构需要修改，需要减少每个sector存储的语音数据（降低存储效率），或使用更大的单元，例如使用8K而不是4K，但增大单元会导致停止录音时丢失较多的数据，目前停止录音时最后一个未写满的Sector是被抛弃的，这种实现方式最简单。
>
> 目前使用的4K单元，在扣除4000字节的ADPCM数据后，再扣除ADPCM state存储，当前Sector的单调增长地址，最终剩余88字节，即存储22个起始点，包含21段。



#### ADPCM数据

C语言格式的ADPCM数据包结构如下

```C
#define ADPCM_DATA_SIZE                  160

typedef struct __attribute__ ((__packed__)) AdpcmPacket
{
  uint32_t major;
  uint8_t minor;
  uint8_t index;
  int16_t sample;
  uint8_t data[ADPCM_DATA_SIZE];
} AdpcmPacket_t;
```



其中`major`是Sector地址，`minor`是0-24的子地址，sample和index是ADPCM解码器需要的编解码状态，最后是160字节的ADPCM数据，总数据包大小是168字节。



如前所述实际上sample和index这两个数据不是每包必要的，只要`minor`为0的（每个Sector的）第一个包提供即可。但同类型的所有的数据包等长更方便一点。

<br/>

### Command

蓝牙连接建立后，客户端应立刻开启Notification，只有开启Notification后写入的指令才是有效的，如果Notification没有打开，固件程序收到写入的指令后直接丢弃，不会执行。



当前固件提供5个指令：

1. `NO_OP`，什么也不做（但可以看一下返回的状态）；
2. `STOP_REC`，停止录音；
3. `START_REC`，启动录音；
4. `STOP_READ`，停止读取；
5. `START_READ`，开始读取；这是唯一一个需要提供额外参数的指令；

执行任何指令后，固件都会返回一个`Status`数据包显示执行命令后设备内部的状态，不额外提供成功失败和错误类型。

#### 编码

| Op Code        | Length | Format and Example                                           |
| -------------- | ------ | ------------------------------------------------------------ |
| NO_OP          | 1 byte | `00`                                                         |
| STOP_REC       | 1 byte | `01`                                                         |
| START_REC      | 1 byte | `02`                                                         |
| STOP_READ      | 1 byte | `03`                                                         |
| START_READ (1) | 1 byte | `04`                                                         |
| START_READ (2) | 5 byte | `04 02 01 00 00`, read from sector `0x00000102` (to sector `recStart`) |
| START_READ (3) | 9 byte | `04 02 01 00 00 04 03 00 00 `, read from sector `0x00000102` to sector `0x00000304` (exclusive) |













