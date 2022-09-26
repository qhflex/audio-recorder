# Button and Working State

## 需求

下图是客户的原始需求。

![button_state_req](button_state_req.jpg)



## 需求分析与设计

下表是状态迁移表。大多数情况都是从`OFF`开始且回到`OFF`，唯一的例外是`RECORDING`可以通过一次长按进入`IDLE`，此时录音关闭。

| state/event | click | double click | press & hold | recordingState | subscriptionOn |
| ----------- | ----- | ------------ | ------------ | -------------- | -------------- |
| OFF         | -     | RECORDING    | IDLE         | -              | -              |
| RECORDING   | -     | OFF          | IDLE         | OFF            | -              |
| IDLE        | OFF   | -            | -            | -              | OFF            |



Button是事件源，它通过Semaphore控制audio和ble模块启动。通知audio模块启动和停止录音的方式不是对称的。通知启动设置`recordingState`，audio模块自己产生启动事件，通知停止使用事件。

Button也是观察者，使用Polling方式观察。

1. 在预启动状态检测double clock还是press & hold，据此决定进入recording模式还是idle模式；
   1. 这是进入recording模式的唯一方式，先设置recordingState，然后post audio旗语；
   2. 如果是进入idle模式，则不设置recordingState，post两个旗语；
2. 在recording状态下，继续检测double click和press & hold，同时polling recordingState；
   1. 如果double click，关机；
   2. 如果press & hold，通知audio停止录音同时post ble旗语；
   3. 如果recordingState false，关机；
3. 在idle状态下，检测click和subscriptionOn的状态；
   1. 如果subscriptionOn，清除累积subscriptionOffTime；
   2. 如果subscriptionOff，累计subscriptionOffTime；如果达到180秒关机；
   3. 如果检测到click，关机；

audio和ble的变更

1. 分开boot sem
2. audio启动检测recordingState，如果为true，启动录音；
3. audio根据设定的最长录音事件自动停止录音，停止录音时修改recordingState为false；
4. TODO 去除其它的停止录音消息，if any.



