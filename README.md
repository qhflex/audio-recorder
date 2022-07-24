# README

This project is reconstructed after the previous one, mems-microphone. The reason for the reconstruction is a widely reported bug was encountered. After around 30-40 seconds, the BLE connection is dropped. It is very annoying to trace down the root cause of the bug. Since the original BLE peripheral example project has no such a bug. And so many thing has been modified in BLE stack.

So a (public) template project is created and features are removed step by step. In each step, it is tested assuredly that the bug is not included. And finally, after most BLE related changes have been made, this project is created based on the template project.

Besides the crucial bug squeezing, the data structures are also modified.