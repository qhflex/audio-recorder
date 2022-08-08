# Audio Format, Storage, and BLE Interface

## 1 文档版本

author: matianfu (at) gingerologist.com

2022-08-07, initial version, draft

</br>

## 2 范围与读者

本文档描述`BLE Audio Recorder`设备固件使用的音频格式、存储方式、和蓝牙低功耗（Bluetooth LE）访问接口设计，包括输出数据格式和客户端可用的指令格式。

本文档不包含固件内部设计与实现的描述、调试和测试方式，这些内容由其它文档描述。

本文档是项目集成和交付的基础文档，所有相关软硬件开发测试人员都应该阅读本文档。

</br>

## 3 音频格式

MEMS Microphone生成的原始数据格式为Signed 16bit PCM（Pulse Code Modulation），客户端软件把音频数据解码后也还原成该格式播放。

考虑到存储容量和BLE传输带宽限制，固件在设备内部存储和输出均使用ADPCM格式，Adaptive Differential PCM。设备仅有一个麦克风，音频数据输出为单声道格式。

固件固定使用16000采样率（Sampling Rate），经测试，该采样率下单独录音存储和单独蓝牙读取均可满足性能要求；固件不禁止同时录音和蓝牙读取录音的使用方式，但该使用方式不在需求范围内，也不保证满足性能要求；客户端开发者应避免两者同时工作的方式。

</br>

### ADPCM说明

ADPCM使用一个近似值查表的方法实现有损压缩数据，压缩比固定为`4:1`，即每个16bit PCM Sample压缩成4bit。

一段PCM样本压缩成ADPCM格式后，除编码的声音数据还需要额外3个byte记录编解码器工作状态，包含一个16bit PCM样本（`sample`，类型为`int16_t`）和索引（`index`，类型为`char`），`index`实际取值范围为0-15，所以有无符号均可。

</br>

ADPCM使用的近似值表有多种标准；固件选择的标准是IMA ADPCM，该标准广泛使用，无需处理器含硬件浮点单元和硬件乘法器，仅需处理器支持bit shift即可。固件使用的IMA ADPCM编解码器源自[Microchip提供源码的ADPCM编解码器实现](https://ww1.microchip.com/downloads/en/AppNotes/00643b.pdf)，该文档包含完整的C代码。

</br>

如果在Windows平台上使用设备产生的ADPCM语音数据，注意Microsoft提供的ADPCM Codec有两种，`XAudio2`里缺省使用的是Microsoft ADPCM格式，不是Microsoft IMA ADPCM格式，如果开发者使用Microsoft提供的Codec，要找到办法配置成后面一种。如何配置开发者自行搜索，固件开发者不熟悉Windows/.net平台开发，无法提供支持。

<br/>

流行的WAV文件格式支持多种ADPCM格式，包括IMA ADPCM格式，但文件头需使用Microsoft定义的扩展（extension）格式；如需把设备产生的ADPCM数据直接拼成WAV文件，使用标准的音频播放器播放（不限于Windows平台），可以采取该格式。同样的，固件开发者无法提供相关技术支持，有该需求的开发者请自行搜索Microsoft提供的文档。

<br/>

与MP3等音频格式不同，ADPCM没有统一的帧（Frame）格式定义。如果一段完整的ADPCM编码的音频数据可保证正确性，帧不是必要的，甚至编解码器状态也不必要，因为`sample`和`index`初始值均为0。

<br/>

实际使用中应用会根据实际要求自定义“段”（Chunk），每个Chunk记录开始时编解码器状态（`sample`和`index`），这样回放时允许从数据中间的某个Chunk直接开始播放，否则只能从头开始解码至所需起始播放位置；在音频数据因各种原因出错时，每段存储解码器状态也可将错误影响限于一个Chunk内。在Microsoft扩展的WAV文件格式里，允许文件自定义Chunk的大小。

<br/>



## 4 存储

设备内部包含SPI NOR Flash，最小擦写单元是Sector（4096 Bytes），固件也使用Sector作为读取时的最小寻址单位。

<br/>

内部实际存储，每Sector存储4000字节存储ADPCM数据，其余96字节存储其它数据；根据前节描述的ADPCM音频格式，编码后的音频数据每样本占4bit，刚好对应8000个样本，是0.5秒的音频数据。即每个Sector存储0.5s语音。

</br>

Flash的实际容量为128Mbits（16M Bytes），但应用开发者该容量无需有假设。从应用的角度看，设备存储抽象为一个单调增长的线性地址空间，从Sector 0开始。每次开始录音时，编码的音频数据写入当前Sector，写满后开始写下一个，依次类推；当所有Sector写满后，固件会从0开始覆盖，但应用程序使用的Sector的地址并不回到0，而是继续增长。用实际的数据举例：实际上Flash有32768个Sector，固件内部保留最后16个Sector做特殊用途，剩余32752个Sector存储音频数据。录音时从Sector 0开始写到Sector 32751时都不会发生覆盖，但写到Sector 32752时，实际上覆盖了物理地址为0的Sector。

<br/>

应用程序使用的Sector地址可以看作“逻辑”地址，而不是“物理”地址。绝大多数情况下，应用程序不用理解“逻辑”地址和“物理”地址的差异，除非应用程序读取的Sector地址数据较早，已经被覆盖，此时固件不会返回错误，而是会自动调整到第一个可读的Sector地址返回数据，或者不返回任何数据直接通知客户端读取结束。

<br/>

如下图所示，当覆盖发生时，录音位置的Sector地址A可以大于32752，包括大于该值的几倍（即发生过多轮覆盖）；此时地址B指向的Sector是最小可读的（最早的尚未被覆盖的Sector），即满足`B + 32752 - 1 = A`；此时如果应用读取地址C（< B）的数据，固件目前的设计不会汇报错误，而是：

- 如果B小于（早于）读取请求给定的结束位置，则从B开始返回数据，数据上会标注实际地址，应用程序可以获知更早的数据都已被覆盖；

- 如果B大于等于读取请求给定的结束位置，读取直接结束，不会返回任何音频数据；

该设计对于只想从头开始获取所有录音数据的情况时是方便的，应用只需提供0作为目标读取地址即可，无需计算哪些地址已被覆盖。

```
0 1 2 ....                                                                                                    
| | | | | | | | | | | | | | | | | | | | | | | | | | |.....| | | | | | | | | | | | | | | | | | | | | | |....
                              ^                           ^                                           ^
                              |                           |                                           |
                              |                           |                                           A 当前写入地址
                              |                           B 当前尚未被覆盖的最小地址
                              C 客户端想读的地址
```

<br/>

## 5 BLE接口设计

### 5.1 Service and Characteristic

Service和Characteristic使用的UUID模板是：`7c95XXXX-6d0c-436f-81c8-3fd7e3db0610`，其中`XXXX`是短ID代入的值，完整定义如下：

- 仅定义一个服务，短ID是`9500`，全长UUID是`7c959500-6d0c-436f-81c8-3fd7e3db0610`；

- 该服务仅包含一个Characteristic，短ID是`9501`，全长UUID是`7c959501-6d0c-436f-81c8-3fd7e3db0610`；
  - 提供`write`和`notification`能力，其中`write`当且仅当打开`notification`时有效，否则客户端写入的值都被忽略。

<br/>

### 5.2 Notification

Notification是固件向客户端程序传输数据的唯一方式。（因为只有一个Characteristic而且未提供READ能力）。

<br/>

固件提供两种Notification数据格式：一种是状态数据（`Status`），客户端写入任何命令固件都会返回`Status`；另一种是ADPCM格式的语音数据（`ADPCM_DATA`），客户端发出读取录音数据指令（`START_READ`）后会获得连续的语音数据包数据返回。`Status`和`ADPCM_DATA`均为固定长度，前者108字节，后者168字节，客户端可根据大小判定获得的数据是哪种格式。

<br/>

两种Notification数据格式大小都超过了BLE最基础的23字节`ATT_MTU`大小（扣除1字节att data size，最大payload为22字节，如果使用LightBlue接受Notification，就只能接受这么多），需要使用更大的`ATT_MTU`。**应用程序建立蓝牙连接后必须先协商MTU**。固件设置的最大PDU为255字节，也是CC2640R2芯片能支持的最大值，使用nRF Connect应用调试可以看到该目标是可以达到的（对应的`MTU`值为251，最大payload为244字节），固件开发者使用Nokia 7 Plus（很古老的手机）和Motorola Edge X30均可达到该目标。如果无法协商到至少175字节的`MTU`，则语音传输无法工作。

<br/>

#### 5.2.1 字节序（Endianness）

注意大于1字节宽度的所有数据类型都使用Little Endian字节序传输，这是BLE的设计习惯（convention），也是绝大多数现行MCU的缺省配置，包括ARM Cortex M。

<br/>

在文档书写上仍使用常用约定，例如`uint32_t`格式的1写为`0x00000001`；在数据传输上，用Byte Array解释时，看到的是`01 00 00 00`，包括使用nRF Connect或者LightBlue调试时，界面上看的都是后者。本文档约定在使用C语言数据格式表述值时均以`0x`作为前缀，在使用Little Endian表述BLE数据传输时，包括发送和接受，均不使用`0x`前缀且每字节分开，就象本段里给出的两个例子一样。

<br/>

注意即使使用Little Endian表达时，包括在nRF Connect或者LightBlue软件界面上看到的Byte Array格式，`01`里的两个字符，仍然是4个most significant bits在前面（即`0`），4个least significant bits在后面（即`1`），换句话说，Endianness没有体现在bit order上，仅仅体现在byte order上。

> 实际上Endianness对bit order是有影响的，在C语言里如果用bit定义结构体内的整数位宽就会发现Endianness也应用在bits上，但是这里讨论的实际上是一个写法（notation）问题，并不是编译器怎么处理bits的问题。所以bit order和endianness的关系和这一段所说的notation约定是无关的。固件设计里没有在C结构体内用bit宽度定义成员的地方。

<br/>

#### 5.2.2 状态（`Status`）

`Status`的C语言结构体定义如下，`recordings`数组包含21个元素，含义后述；`Status`数据结构的总大小为108字节，共包含27个`uint32_t`类型数据。

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



`flags`目前仅使用最低的两位；最低位表示`recording`，设为1表示当前正在录音；次低位表示`reading`，设为1表示当前正在读取录音数据。因为设备只接受一个BLE连接，实际上客户端是知道`reading`状态的，在`Status`里包含该信息主要是方便调试。

<br/>

在设备内部，录音和读取录音数据是两个相对独立的部分。`rec`前缀命名结构体成员都是和录音有关的状态数据，`read`前缀的成员都是和读取录音有关的状态数据。

- 录音
  - 如果当前正在录音，即`recording`标记为1，`recStart`是当前这段录音的起始sector地址；`recPos`是当前正在写入录音的sector地址，均为逻辑地址（下同）；
  - 如果当前不在录音，即`recording`标记为0，`recStart`和`recPos`是相等的，指向下一次录音开始的第一个sector地址；
- 读取录音
  - `readStart`是当前读取命令给的原始参数，该参数不会自动调整，即使它小于当前尚未被覆盖的最小的Sector地址；
  - `readPosMajor`是当前在读取的Sector地址；
  - `readPosMinor`是当前在读取的Sector内的读取单元的index；一个Sector有4000个字节音频数据，每个ADPCM数据包只包含160个ADPCM数据，所以`readPosMinor`的实际取值范围是0-24，即4000字节的数据要拆成25个包发送；

<br/>

`Status`提供的读取录音状态主要是为调试方便，没有业务需求要求客户端软件显示或使用`Status`内和读取录音数据相关的状态。录音状态是客户端会需要使用的，客户端与设备建立蓝牙连接时，设备可能已经在录音，客户端程序还可以根据`recStart`和`recPos`判断当前录音已进行多长时间。

<br/>

#### 5.2.3 录音分段信息（Recordings）

`Status`里的`recordings`数组，算上`recStart`正好定义了最近21条历史录音记录的起始和结束的位置；可以把`recStart`看作`recordings`数组的第22个元素。



设备开始使用时，`recordings`数组元素全部初始化为`0xffffffff`，表示这一条记录没有值。`recStart`在初始化的时候设置为`0`，如下表所示：

| name                        | value        |
| --------------------------- | ------------ |
| `recordings[0]`             | `0xffffffff` |
| `recordings[1]`             | `0xffffffff` |
| ...                         | ...          |
| `recordings[19]`            | `0xffffffff` |
| `recordings[20]`            | `0xffffffff` |
| `recordings[21] (recStart)` | `0x00000000` |



如果第一段录音写满了5个Sector后录音结束，`recordings`和`recStart`会变成如下表所示状态，表示目前有一段录音，从地址0（包含）开始到地址5（不包含）结束，依次类推。
| index                       | value        |
| --------------------------- | ------------ |
| `recordings[0]`             | `0xffffffff` |
| `recordings[1]`             | `0xffffffff` |
| ...                         | ...          |
| `recordings[19]`            | `0xffffffff` |
| `recordings[20]`            | `0x00000000` |
| `recordings[21]（recStart)` | `0x00000005` |

该表可以看作每次录音结束后所有记录『上移』一格，`recordings[0]`的数据被抛弃。任何相邻的两个元素记录了段录音的起点（包含）和终点（不包含）的位置。

<br/>

受资源限制，固件程序不维护这个列表的数据有效性，应用程序要考虑这样一些情况：

1) 录音分段中的音频数据如果已经被覆盖，固件不会修正`recordings`；应用程序可以根据读取ADPCM数据的实际结果自行处理；
2) 一个录音分段的开始部分音频数据已被覆盖，尚有部分未被覆盖的情况是完全可能遇到的，应用程序需要自己判断这种情况是否已经发生，并和需求方协商如何应对；最极端的情况是持续的超长时间录音，比如连续录音超过几十分钟，会导致`recording`记录只有最后一条是可用，且其起始地址可能已经被覆盖，应用程序要定义这种情况下程序的行为；
3) 如果每次录音时间很短，比如只有几秒钟，`recordings`很快就会被填满；被填满不意味着前面的录音数据丢失，应用程序仍可读回所有数据，但分段信息已经丢失；

> 如果要克服3所述的问题，当前的数据结构需要修改，需要减少每个sector存储的语音数据（降低存储效率），或使用更大的单元，例如使用8K而不是4K，但增大单元会导致停止录音时丢失较多的数据，目前停止录音时最后一个未写满的Sector是被抛弃的，这种实现方式最简单。
>
> 目前使用的4K单元，在扣除4000字节的ADPCM数据后，再扣除ADPCM state存储（4字节包括对齐），当前Sector地址（4字节），最终剩余88字节，即存储22个起终点，包含21段数据。

<br/>

#### 5.2.4 音频数据（`ADPCM_DATA`）

C语言格式的`ADPCM_DATA`数据结构如下

```C
#define ADPCM_DATA_SIZE                  160

typedef struct __attribute__ ((__packed__)) ADPCM_DATA
{
  uint32_t major;
  uint8_t minor;
  uint8_t index;
  int16_t sample;
  uint8_t data[ADPCM_DATA_SIZE];
} ADPCM_DATA;
```



其中`major`是sector地址，`minor`是取值范围0-24的`packet index`，`sample`和`index`是ADPCM编解码器需要的编解码状态，最后是160字节的ADPCM编码数据，总数据包大小168字节。实际上`sample`和`index`这两个数据不是每个包必要的，只要每sector第一个包（`minor=0`）提供即可；但使用等长数据包更方便一点。

<br/>

### 5.3 指令（Command）

蓝牙连接建立后，客户端应立刻开启Notification，只有开启Notification后写入的指令才是有效的，如果Notification没有打开，固件程序收到写入的指令后直接丢弃，不会执行。



当前固件提供5个指令：

1. `NO_OP`，什么也不做（但可以看一下返回的状态）；
2. `STOP_REC`，停止录音；
3. `START_REC`，启动录音；
4. `STOP_READ`，停止读取；
5. `START_READ`，开始读取；这是唯一一个需要提供额外参数的指令；

执行任何指令后，固件都会返回一个`Status`数据包显示执行命令后设备内部的状态，不额外提供成功失败和错误类型。

<br/>

#### 编码

| Op Code          | Length | Format and Example                                           |
| ---------------- | ------ | ------------------------------------------------------------ |
| `NO_OP`          | 1 byte | `00`                                                         |
| `STOP_REC`       | 1 byte | `01`                                                         |
| `START_REC`      | 1 byte | `02`                                                         |
| `STOP_READ`      | 1 byte | `03`                                                         |
| `START_READ` (1) | 1 byte | `04`                                                         |
| `START_READ` (2) | 5 byte | `04 02 01 00 00`, read from sector `0x00000102` (to sector `recStart`) |
| `START_READ` (3) | 9 byte | `04 02 01 00 00 04 03 00 00 `, read from sector `0x00000102` to sector `0x00000304` (exclusive) |



`START_READ`提供了3种格式，前面两种可以看作第三种的简略格式；第三种格式需提供读取录音的起始sector和结束sector的地址（起始包含结束不包含）；第二种格式不提供结束值，结束值被自动设定为`recStart`，即最后一段已结束的录音的结束点；第一种格式不提供起始和结束，起始会被当作0处理，并自动调整到最早的未覆盖录音数据发送；结束点会自动设定未`recStart`。使用第一种格式，如果不发生传输中断的话，可以一次型提取设备内所有录音数据。

<br/>

## 6 总结

1. `recordings`应视作是一个“辅助”信息，`START_READ`提取录音数据实际上没有体现有录音分段信息存在（例如自动在某个分段边界上结束），客户端需主动提供读取的结束点；
2. 固件不维护`recordings`的数据有效性；
3. 早期录音分段信息丢失和语音数据被部分覆盖都是客观上会出现的情况，应用开发工程师需和需求方探讨如何处理并给出行为定义。









