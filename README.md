
# DX12 Work Graph Sandboxing

This is a simple prototyping project to explore and familiarize myself with DX12’s recent Work Graph feature based DX12 samples.

## General idea

Explore Work Graphs by implementing a simple screen space filter for an image. This could be a valid practical use case for Work Graphs: many of e.g. denoising filters consist of several consecutive filtering passes that require reading/writing of the temporary results between passes and thus making the passes extremely bandwidth limited. Using Work Graphs could help with optimizing this bottleneck. Furthermore, it’s often the case that desired spatial radius of the filters vary significantly across the screen space and thus more granular work dispatching using Work Graphs could provide solid performance gains.

## What’s done?

* Overall backend to start working with Work Graphs, including:
* Simple project structure
* Added imgui etc. since DX12 sample was purely command line app
* Image loading (imgui, std_image)
* Simple Work Graph with necessary resources and a single node that copies the input texture to UAV

## TODO

* Make the Work Graph actually meaningful, adding nodes etc.
* Fix some non-released resources
