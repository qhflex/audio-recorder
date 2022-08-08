# 使用nRF Connect调试

## About nRF Connect

nRF Connect是Nordic公司发布的一款App，在Google官方的Google Play可以下载到。可能因为Nordic和TI是BLE领域的主要竞争对手，TI所有开发测试文档都未提及该应用，而是另外推荐一款第三方软件LightBlue用于调试BLE程序。

LightBlue操作简单，界面设计也更直觉友好，但对于BLE Audio Recorder项目，LightBlue无法支持协商MTU，所以无法使用。除协商MTU之外，nRF Connect还支持选择PHY，还可以LOG全部传输数据，这些都是对BLE Audio Recorder调试很有帮助的。

本文档叙述使用nRF Connect调试的方法和注意事项。

<br/>

## 打印输出

当前开发的固件支持3种打印输出方式，其中两种用于打印ADPCM语音数据，一种用于打印设备行为。支持3种方式是因为CC2640R2的片上资源太少，无法同时支持多种模式，每次只能通过编译开关编译支持其中一种。

出于功耗考虑，最终用于产品的版本不会使用任何一种打印输出。但配合nRF Connect调试时，使用打开打印设备行为的固件会更方便看到设备工作状态。

<br/>

