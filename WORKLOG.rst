2022-09-30
----------
With the recent interests in Textual Inversion, it seems implementing model freeze (such that, not only parameters are not updating, but we don't allocate space and do gradient computations for these at all) are beneficial. Textual Inversion only cares about the input guidance and use gradient descent to find better input vector, the model itself not updating. Thus, by "freeze" the model, we can potentially saving quite a bit of memory usage that way.

Also, I need to dig deeper into MPSGraph, seems Metal buffers are not contiguous (as flat memory space), so the memory allocation algo probably need to retrofit that requirement (or have some other clever ways to do it?).


2022-08-28
----------
There are always edge cases need extra care even if the core is pretty stable. The latest case is the fact that for models, I optimize too aggressively and when we change outgrad from some to none and back, while minimizer changes, we could end up with a case where the saved_aux reset to 0. This is undesirable for Adam optimizers or other stateful optimizers. An example would be:

 1. Evaluate a model with inputs;
 2. Evaluate a model with inputs with different batch size, but these inputs marked as constants (thus, outgrad is disabled);
 3. Set minimizer to Adam;
 4. Evaluate a model with inputs as variables (thus, outgrad re-enabled);
 5. Set minimizer back to noop.

When repeat above step, on step 1, it absorbs with new input batch size, all is good. On step 2, because inputs marked as constants, we need to re-init the grad. Because grad is re-init'ed, previous optimization when noop to Adam, saved aux won't change, but because the re-init'ed, noop won't have the same saved aux size as Adam. Therefore, on step 4, we will end up with saved aux reset because we think these saved aux are new.

I haven't figured out a fix yet. But it is likely going to make gradient init more stateful.


2021-04-14
----------
Even the core part of NNC has been stablizing for close to one and half years now, and no serious bugs discovered for close to a year, you can never say it is rock-solid. It simply means that high-frequency bugs are squashed, and low-frequency / more frustrating bugs are now need to be fixed. Thus, frequency-wise, I probably only encounter a bug couple of months, and can be easily worked-around. So far, I am determined to fix these rather than introduce workarounds. Once workaround introduced, the code will be much harder to document and reason about. It is a trade-off that I am under no pressure to make.

So, I've spent the past couple of days to chase down a bug with simple workaround. It starts in last Saturday. Whenever I upgrade CUDA Toolkit, I will verify if there are any performance regressions. I have integration pipeline setup to test correctness on each commit with CIFAR-10 and IMDB sentiment analysis (a transformer model) to get semi-real-world results. For performance testing, I bring the big guns: ImageNet and WMT models. These models took a full day on 4x2080Ti to train, but it is easy to see throughout quickly and validate if any performance regressions exist.

However, this time, WMT training crashed.

I switched WMT training to fully asynchronous a few months ago. This crash is related to that. Now, backup a little bit. In NNC, computation can be expressed asynchronously through stream / signal abstraction. It works very much like CUDA's stream / event abstraction and gives the underlying runtime full knowledge to schedule things optimally. It doesn't mean this will run fastest, but it does give the underlying runtime ability to do so. When I say "full asynchronous training", it just means the training procedure dispatched to a stream so everything happens on the CPU can overlap with everything happens on the GPU. Previously, I had a few "sync-points" within `ccv_nnc_dynamic_graph_evaluate`. A few months ago, these are all removed.

The crash is not hard to fix. CUDA is not happy with the way we recycle events, and it happens that we can actually reuse the same event again and again as long as wait happens immediately after emit. The fix actually simplified code in various places.

However, the accuracy doesn't climb any more. Closer inspection shows the training will result nan after a few iterations.

I am pretty sure it works before. Hence, the bug shouldn't be in the training code. Inserting a few sync points quickly identifies the culprit should be asynchronous scheduling.

Asynchronous scheduling bug is hard to fix. Well, it is reproducible. But enabling debug output would synchronize streams, and the bug will be gone. It also doesn't help that WMT is not a small model. Scheduled on 4 GPUs, the model dispatches thousands of kernel launches on each iteration. First, I need to identify exactly what's wrong.

This actually took most of my time. Haven't debugged this part of code for quite some time, I played fast-and-loose early unfortunately. By suspecting it had something to do with the full asynchronous `ccv_nnc_dynamic_graph_evaluate`, I spent a day or two tried to reason whether there were any issues in the stream synchronizations. It involved a lot of `printf` (debugger likely will mess with asynchronous scheduling) with no luck.

Although no root-cause, during that process, I did come to one thing valuable. I narrowed it down to the interactions between forward pass and backward pass. Inserting sync points prior to forward pass (so data loading is not the culprit) and after the backward pass (so applying gradients is not the issue) doesn't help this issue at all. However, inserting sync point in between forward pass and backward pass makes it go away instantly. I also narrowed it down to a reproduce case that doesn't involve "running it for 5 minutes to see if accuracy climbs". This reproduce case also provide a little bit more insight: after 5 iterations or so, the accumulated gradients on GPU 4 has 10x larger value than these on GPU 1, 2, 3. Because I can only observe the accumulated gradients (remember, if I print gradients out directly in backward pass, it implicitly synchronizes the stream, and the bug will go away), it could be an issue with the previous gradients in the backward pass, or memory reuse issue in gradient accumulation phase (WMT model accumulates gradients in multiple iterations and then apply gradients descent).

After sitting on it for couple of hours again, and no obvious issues found with memory reuse (the accumulated gradients locations are pretty exclusive, due to how they are organized), I need to bring the Bazooka to fight this bug. Note that I always have an easy way out, just re-insert a sync point back into `ccv_nnc_dynamic_graph_evaluate` and call it a day. If this is not a passion project, that may be what I will do. But I want no regrets. NVIDIA has a Visual Profiler, but I haven't used it for a year now. It doesn't open any more. Nsight is its successor. These profilers will help you to visualize the exact order of kernel executions on the streams. If there are any mess up, it should be visible there. In theory, I can generate similar graphs with Catapult by inserting callbacks into streams myself. I would rather spend that time when this bug is fixed. Also, that assumes my first profiler implementation in NNC is bug-free. Too big of an assumption.

Even unfamiliar with Nsight interface, but by going over the event view, I start to understand what these does. There are a lot of kernel launches from CUBLAS or CUDNN and these are harder for me to understand (NVIDIA really should open-source these!). Starting with kernels I wrote was much easier to orient myself against thousands kernel launches. Luckily, the backward pass of softmax op is done by me. Finding this kernel launch shows me what's before / after. I navigated to event view of GPU 4, because that is what has the large accumulated gradients.

The events before and after looked annoyingly noisy. But patience eventually revealed that a `dropout_bp` kernel launch happened milliseconds before softmax backward pass. That shouldn't be the case, softmax backward pass should precede any other backward propagation pass. I inserted sync point, collected events again, and this round, no such thing happened. Removing sync point, the order was messed up again.

This identifies the issue with the ordering, but not exactly pinpoints what's wrong with the code. It still can be a coding error in `ccv_nnc_dynamic_graph_evaluate` asynchronous execution path. With some luck (and debug logs when VERBOSE enabled), I found something strange. When starting the backward pass from `ccv_nnc_dynamic_graph_evaluate`, I wait stream 0 from stream 1, 2, 3 (these should from different GPUs?), but the actual softmax backward pass kernel launches on these GPUs happened on stream 0, 2, 3, 4. If stream 4 never waited, `dropout_bp` will happen before softmax backward pass on GPU 4, it matches the previous reproduce case really well.

It becomes pretty obvious when I checked how I actually do the initial waits. First, a little bit more background. A compiled graph doesn't automatically have streams scheduled on it, you need to explicitly schedule it. Scheduling the whole graph can give a default schedule object, and you can run the graph with that. You can also schedule a part of the graph, with given sources and destinations on the graph. This will return a schedule object that you can run the partial graph with. In the beginning of running any graph, I have a list of streams that need to wait. This makes sure that any streams cannot be started until the given stream (as parameter) synchronizes. The list of streams that need to wait is simple to get. Just iterate over all sources and find their stream indexes.

So far, so good. However, I got the list of stream indexes before the "rebinding" process. Because for the whole graph, we have a list of actual allocated streams. For partial schedules, we don't want to allocate new streams (that can impose more synchronization headache). Thus, partial schedules will reuse the allocated streams (and the allocated streams will go with the graph, rather than the schedule object). The initial stream indexes won't differentiate these cases, and later we need to find the right stream indexes from the allocated streams to "rebind" them. It is done for all stream references except the "list of streams to wait from the beginning". It happens in this case, allocated stream 0 and 1 are all on GPU 0, therefore, we need to "rebind" partial schedule's stream index 0, 1, 2, 3 to allocated stream 0, 2, 3, 4.

Do the "rebinding" and the bug is fixed. No workaround needed.


2021-03-14
----------
Looking at Jittor's source code, there are some shortcuts they've taken that are interesting. It seems their IR parsed from the source code, which looks like a combination of some template language and C++ code. I choose a different path where the IR is directly constructed, so no parser here.

That has been said, the first step is to implement a simple, but correct interpreter such that I can actually run in the slow mode to verify optimization passes are implemented correctly. I probably need to restrict what data types supported for the interpreter so it can be implemented with reasonable confidence.

This micro op implementation would probably take another month to finish. It definitely exceeds my previous estimation of days work :)


2021-01-10
----------
Continue yesterday's discussion, I removed the last sync point (inside the `ccv_nnc_dynamic_graph_evaluate.c`). There is a small bug in static_schedule method such that if there are multiple starting points in a graph, we didn't sync all them to the given stream, thus, causing race issues.

The last removal is rather restrictive, it requires the graph for `ccv_cnnp_model_t` has no suspension point. Like we discussed yesterday, that will work today, but won't work once we introduce control structures. Having suspension point will not be a major concern from code structure point of view (at the end of the day, that only requires us to make several existing methods, such as `ccv_cnnp_model_evaluate` has coroutine alternatives.

The major concern comes from memory management. Let's assume the simplest case, where for dynamic graph, there won't be embedded control structure (that would be a bit weird to support static control structure within a dynamic graph directly), but for models, there could be embedded control structures. In that case, you need to make: async counterparts for `ccv_nnc_cmd_exec`, `ccv_cnnp_model_evaluate`, `ccv_cnnp_model_backward`. But that is not over. Because these are async now, `stateful_exec` need to be manipulated before any suspension points to make its lifetime predictable. This is actually not possible if we don't force a sync point after apply gradients. This is because suspension points accumulates, so if we suspend upon `ccv_cnnp_model_backward`, the next `ccv_nnc_dynamic_graph_evaluate` call will be suspended until previous `ccv_cnnp_model_backward` finished. To avoid such accumulated suspension points, we need to either sync, or reference counting the `stateful_exec` object.

It also means we need to reference counting the tensor objects, because we will use tensor objects after suspension points for `ccv_cnnp_model_evaluate` etc, while the dynamic graph won't guarantee the lifetime of these tensor objects (they may be freed). This is never an issue before because previously, our async schedule happens before these tensor objects' lifetime ends. While the async streams still need the memory regions referenced by these tensor objects, they don't need the metadata. These memory regions associated and recycled per stream, hence, no data races.

 * suspension point: I use this word to describe co_await / co_yield and its variants in the code, at which point, the coroutine yields control back to the scheduler for which the scheduler can later resume. Currently, with careful design, there is no suspension point in `ccv_cnnp_model_t` or `ccv_nnc_dynamic_graph_t`, but that can change once we introduced control structures.


2021-01-09
----------
Spent some time to see if I can make the dynamic graph async operations work better. Previously, the async operations on the dynamic graph has a few sync points: when finishing backward, when finishing apply gradients, we forced it to wait. The reason is because we cannot free buffers until computations are done.

I did a few commits in the past a few days to fix this issue. There are quite a bit back and forth and there are still issues, will document what I have done, and what works left, and why it is difficult to solve in C.

The async operations in nnc follows largely with CUDA's stream / event concept. A stream is a serial execution engine you can dispatch operations to it, and event is used as synchronization mechanism between different streams. However, you can only wait for an event when it is signaled already on a different stream. Thus, stream 1 has to signal event A first before stream 2 can wait for event A's completion. This means we have to schedule everything upfront.

This messes up if you have control flows, such as while loops and case..of. To make this work in static graph, I devised a coroutine based solution that works fairly well in that context. When you co_stream_await on a CUDA stream, it will only continue the execution when the stream reached that point, and the subsequent tasks only be scheduled after that. In this way, the order of event signaling / waiting is not messed up.

This breaks down when we have dynamic graphs. With a single stream, it sort of still works, when work with care. We just dispatch on the stream as we go, and even for backward and apply gradients, it should work because there is no control structure. That is sort of where I am at right now. If you structure this carefully, it can work with single stream.

The past a few commits made the `ccv_nnc_stream_context_add_callback` work as expected, i.e., a callback will be triggered, safely when an execution point reached, no matter if there are coroutine executions or not. This helps to get deallocating graph / tensor arena correctly for backward / apply gradients method. Thus, help to lift the sync points there.

Then it gets muddy. It works because there is no coroutine hangs, by accident, during dynamic graph execution. If there is, the backward / apply gradients will still execute correctly, because it happens to support coroutines when it runs internal static graph. However, subsequent dynamic graph execution won't, because it naively dispatch to the stream directly, without coroutine waits.

It gets worse. Right now, we haven't lift all sync points. When a model evaluated, we need to wait for its execution stream, and all the neighboring stream to finish, before continue. Why? Because the model evaluation is done inside a custom command, and that custom command won't get the right scheduler to do the right waiting when executing.

If there is any coroutine suspension point, our current schema falls apart. For one, `stateful_exec` won't have valid lifetime. Another, it will be problematic to call `ccv_nnc_cmd_exec` because it doesn't respect coroutine scheduler at the moment as well.

So, the choice is simple. Either I don't support coroutine anywhere in dynamic graph / model, so it schedules everything on the stream, or I have a good coroutine + lifetime management support everywhere so I can infect everything with coroutine. The downside of choice 1, obviously, is the inability to support control structure in model any time soon (run control structure requires coroutine suspension points).


2020-09-10
----------
I mostly developed ccv / nnc as a monorepo. Since I started to use nnc for other projects as the backbone, it becomes obvious now that the monorepo development works fine for smaller demos such as object detection, natural language processing, for small / medium project, I don't want to clone ccv and start development there. I've gained some experiences using Bazel with Dflat project, therefore, it seems natural to have ccv / nnc to support Bazel.

There could be some circular dependencies down the road, since the longer-term plan is to have ccv uses nnc for many applications (object / keypoint detection, SLAM etc.), but for now, there shouldn't be any.

Another issue is the configuration. Core ccv / nnc can be compiled without any dependencies, but to function with GPU, or multi-threading, we depend on some other libraries. The feature detection need to generate proper .bazelrc file and use `config_setting` throughout. There could be some problems with CUDA / nvcc as well.

Once the Bazel support is done, I can start to do the most exciting project for a while - Swift interop.


2020-01-12
----------
Memory reclamation is not as simple as what PyTorch made it out to be. The simple scheme PyTorch uses is to allocate memory gradually, and only do a pause / collect (because you have to synchronize with all devices) when run out of the memory. It is only useful if "all" your memory allocation go through the same path, or you won't have multi-processes.

In my case, what bites back is the workspace memory for streams. Each stream can maintain and allocate their own workspace memory. These memory bounded to the stream and never reclaimed until stream destroyed. This simple scheme works fine for static graph. However, now it will conflict with the dynamic graph because dynamic graph won't release the memory.

So, the choice has to make now is whether to have a "global" memory allocator for streams as well, that shared with the dynamic graph. Or inject a custom allocator to streams. I probably would prefer later consider this is a library not a framework.


2020-01-06
----------
Get myself more familiar with LLVM. I am surprised the design separation of Function v.s. Basic Block v.s. Instruction, and then fact that Basic Block itself is not recursive. The loop structure, in particular, loop-closed SSA form is not something intrinsic to Basic Blocks. If the design is more functional, there shouldn't be a separation of Basic Block and function, while Basic Block would be enough to express loop structure. What I do learnt though, is how easy LLVM is to manipulate BB / Func / Inst through CGF / CGM. Comparing to how hard to create a phi node inside nnc (not explicitly, through the mapping when add case..of sub-graph), or assigning loop carry-overs, LLVM is so much easy to remove a BB, create a BB, and hook up one BB with another. Not to mention to iterate over Inst and BB, it is something builtin while there is still no easy way to iterate over nodes and manipulating them at the same time inside nnc.

While it is very inspirational, I will punt more work in defining a better symbolic graph interface. After all, Relay and MIIR all try to do better job at expressing computation graph, I can learn one or two from their experimentation first.


2019-08-22
----------
Implementing named models and proper tensor init seems not so easy. Particularly, for complex training setup, such as: having new model share some weights with simpler models (for example, seed ResNet101 with ResNet50 parameters), or fix the training on certain weights, and continue on the others. The former one requires us to keep some consistency between different models, the second requires us to mark the model somehow while adding trainables.

Thus, we should be able to name a given model (or layer). The trainables weights will be fixed to that name, thus, adding new layers won't impact the old weights, and these can be loaded successfully. To accomplish this, I added the new ``ccv_nnc_tensor_read`` and ``ccv_nnc_tensor_write`` methods to keep tensors. This also marked a departure for how persistence should be done. Rather than ad-hoc with SQLite, it will all be marked, now with tensor and names.

Persistence worth a rethink in general, it starts by just names and tensors. I will remove persisting symbolic graph support. Instead, will enable persisting graph and tensor arena.


2019-08-12
----------
Revamp the persistence for networks. Comparing to other solutions such as protobuf, I would rather just use SQLite. But it will be different from previously I do this. Previously, when I use SQLite as persistence, it is not composable. Thus, different algorithm will use SQLite differently, there is not shared schema. The revamped way will have all tensors saved into the "tensors" table, and everything else reference to it by name. For example, for CNNP, there is no persistence other than "tensors", the model itself is not persisted at all. However, for tensor arena / concrete graph, we will persist both the tensor allocation, tensors and the graph. I don't think we want to persist symbolic graph any more. It is likely I will delete that code later.

In this way, one can query the SQLite and navigate the database as if it is just a "workspace" file (in Matlab sense). These data can be easily ported to pandas or other places because you only need to write a tensor loader once, everything else just a naming convention afterwards.


2019-07-15
----------
Moved to SF. It seems Nesterov is important for ResNet-50. Moved to Nesterov, the final result is much more comprehensible.

I am currently working on a concept called LSSC (Linear Scaling Spatial Compression). The insight is simple. Unlike weights, activations have more spatial redundancy. These activations get used during back propagation. It is conceivable if we can have some way to compress the activation, and during back propagation, decompress these activation back, we can save some amount of memory. Given these kind of compression ratio (Bitmap to JPEG etc.) are surprisingly high, we can expect a big reduction in memory usage if the compression scheme used during training process. Currently, I am prototyping this, the big unknown is the quality of the compression (I am pretty confident about this, because the decompressed activations only used during back propagation anyway), and speed (I am more worried about this, because it is unclear how to implement this efficiently on GPU).

Stay tuned.


2019-05-31
----------
Weight decay as the regularization has to be one of the most non-obvious thing in my implementation. The theoretical background for weight decay is to minimize weights, thus, loss^{wd} = loss + c * sum{||w||^2}. Thus, the selection of c would be important. Somehow in the CIFAR-10 implementation, I choose a very aggressive c. In implementing imageNet, that bites me. Too aggressive c makes the weight too heavily regularized, therefore, cannot converge on larger dataset such as imageNet unfortunately.

I think this is time for me to implement RMSProp or ADAM for faster iteration. Hyperparameters for SGD are too much and not universal.


2019-05-28
----------
Debugging memory related issues is hard. I've been battling against a bug when loading trained ResNet model into memory and continue the training, it will mysteriously halt at certain GPU operations. Debugging GPU related issues is always difficult. It often involves first identifying exactly which CUDA API call failed (that is why you see the codebase littered with ``CUDA_ENFORCE``, ``CUBLAS_ENFORCE``, ``CUDNN_ENFORCE``, ``NCCL_ENFORCE`` to make sure we fail early).

This time it is relatively easy. The fault command is the softmax fused cross entropy loss backward op. However, because it only happens when I enabled parallel mode, I was confident this is somewhat related to I haven't ``cudaSetDevice`` properly in some methods. Furthermore, if I moved weights loading after the data prefetching, it seems all worked. Thus, I've been trying to identify which function call happens on which GPU device for extended time with no progress made. A lot of assertions added but no bug was caught.

Then when searching for 700 error ``cudaErrorIllegalAddress``, I came across `cuda-memcheck`. It is a little nice tool very much like `valgrind`, it is plug-and-play. With `cuda-memcheck`, within minutes, I identified the illegal memory access (related to how we handle fp16 the same as fp32 when copy value over). It also helped me to identify a double-free bug as well.

It seems reasonable to say that I need to include `cuda-memcheck` in the buildbot script to help protect against memory issues from GPU side in the future. Definitely a good learning experience today.


2019-05-22
----------
Besides lacking of debugger.

Without debugger, currently, to run cnnp programs, there are several issues.

 1. Ad-hoc looking at GPU tensors and getting statistics are hard (this is partially addressed by having GPU tensor's first 3 values in the VERBOSE output now, but we don't have statistics);
 2. There are issues with nan if the learn rate is too large (of course!). Since GPU is running asynchronously, it poses challenges to scream at the point when we hit nan, and give enough trace to look back to see whether it is because we have some faulty ops, learn rate too high, initial gradient is too much (not an issue until we implement non-1 gradient propagation, this is useful to increase / decrease scales for fp16);
 3. Extract loss / accuracy from the data is far from obvious. I need to manually transfer the data to the CPU, and write some code to collect the accuracy;

There are several ways to do this. I can have a stats function that given a list of tensors, generate statistics (min, max, average, std), and then transfer these stats back to CPU for inspection. This requires to modify the graph, but could be relatively easy. To gather accuracy would actually be harder. For one, we use one hot, and later we are going to use mixup, which means the ground truth is actually not inside cnnp itself. Not to mention we want a way to extract accuracy from cnnp when evaluate against test set.

Stats are fine, we can have assertion enabled mode and assertion disabled mode which will be faster but no protection from abnormal stats. Accuracy seems to be something you need to track over time, therefore, the overhead need to be very low. I think the asynchronous execution nature on GPU really makes the debug process harder. Maybe we should call this debug mode, where we always copy out the tensor stats.

Another thing, is to backtrack and restart from a given epoch. We currently cannot do that because the checkpoint file gets consistently rewritten. We don't keep a journal of the checkpoints, thus, we cannot restart from a given checkpoint. This shouldn't be that hard, it just feels like something we can leverage SQLite, but it is not obvious how (SQLite supports WAL and MVCC, but that is for different use cases).

BTW, the ``ccv_resample`` method seems to be broken and can end up with nans. I need to dig into why (it seems from CUBIC, but I need more data).


2019-05-14
----------
Autotune implementation needs some work.

I didn't spend much time on autotune. It only surfaced this issue when I tries to implement the fp16 support. The original issue is from cudnn's ``cudnnGetConvolutionBackwardDataAlgorithm`` method. For fp16, this method will return a wrong preferred algorithm, thus, failed the following operation. The find method doesn't have this bug. That triggered me to look into why the ``cudnnFindConvolutionBackwardDataAlgorithmEx`` method is not called because it is part of the autotune process.

It turns out that there is a bug in the ``ccv_nnc_graph_autotune`` where given 0 sources and 0 destinations, it doesn't run the full graph. Then there is a bug in the convolution's autotune implementation where given 0 workspace size, it will skip the autotune completely. On top of that, we cannot really use the autotune as it is on the complete graph. The autotune process will run the command multiple times against different backends, therefore, if the command is not idempotent (it shouldn't), this will contaminant the final output.

I think the proper autotune implementation should allocate some inputs and outputs. When autotuning, copying the original inputs over. This can be repeated as much time as you would like. The only gotcha: there are some commands require inputs and outputs to be the same (enforce_inplace), that allocation need to handle this as well.

As of now, I workaround this problem by only autotune until backward finishes, and the autotune function avoid repeat too much times by identify there is only one backend. It is not as ideal.


2019-05-09
----------
I don't know why my graph traversal code doesn't properly address "don't visit nodes that not contribute to the destination". Initially, how the graph was driven done with flood fill.It is all fine until I want to get more serious.

The compounding problem is that I want to, eventually, making the concrete graph computation as fast as do the computation directly (even if the tensors are as simple as scalar (0-dimension tensor)). That means have a more compact representation of the graph, better interpreter (right, you can think the ``ccv_nnc_graph_run`` as "interpreting"), and doesn't do topsort every time.

Unfortunately, that's the absurd world I am in now. Right now, if a graph is not ``ccv_nnc_graph_static_schedule``, running it requires to traverse the graph 4 times: 1. Collect statistics about how many incoming edges for each node; 2. Collect exactly which are the incoming edges; 3. Reverse traverse from destinations to the sources, marking node that can be reached this way; 4. The final traversal, only call node that is marked in step 3. All these is because I don't want the graph representation including both outgoing nodes and incoming nodes. Including incoming nodes is obvious but a struggle for me because I don't want to maintain two sources of truth about the graph structure. Then, I end up with this 4-pass graph traversal.

There are ways to optimize this though. First, let's be honest, flood fill won't give me efficient interpreter. I need the topsorted result available already to be efficient. It seems more and more likely, that "cache" topsorted result thing could be another layer "cache" the opcode for graph interpreter. Very interesting.

After 3 months with the new machine built (4xRTX2080Ti), and fixed the AMD freeze issue, I finally can work on the fp16 support again. Long time indeed!


2019-05-06
----------
Designing API is hard.

This can be seen by the expansion of ``ccv_nnc_symbolic_graph_minimize`` parameters. Previously, the parameters are a lot, but makes sense. The parameters you try to optimize, the minimizer, the losses, and the sources / destinations for the graph. The output from this function is the list of gradients, updated parameters. However, it is not flexible enough for the case where I need to compute the gradients against input, but not necessarily create ops to "optimize" inputs. This is expected to implement outgrad support for ccv_cnnp_model in multi-stage mode. Otherwise, we need to essentially reimplement the minimize function (i.e., first compute gradients, and then insert minimizers). For this case, on the API side, I added additional parameters called inputs, which is the tensors we want to compute gradients, but not optimize for (not free parameters). The side effect, as you can see now, is a more complex API.


2019-05-05
----------
Debuggability in framework is a big issue. There are a few things I should do earlier but haven't that bites me now. One example is how we handle symbolic graph compilation. When it works, it is pretty cool, but when it doesn't, there are some hard time to look through what's going on. Example: 1. When a tensor is used before initialization, we didn't provide init with some harder value (nan). This is simple to solve though, as long as we do that initialization when create tensor arena; 2. Wish this is as that simple, tensor areas are reused, thus, it could be uninitialized but with some value in it already, this may be solved if we force to init some values (using ``CMD_SET_FORWARD``), but that has consequences such as violate SSA during the compilation; 3. That leaves me to conclude that I really should do the simple allocation implementation much earlier, which is the debug mode for our tensor reuse logic, as well can be coupled with default initialization mode. In this way, each new tensor will be allocated from the heap directly without reuse, and set default initialization value. This helps to check reuse logic (however, less useful since our reuse logic is really robust nowadays), but also, makes the uninitialized tensor case much easier to surface. This mode however, is not simple to implement now, because additional tensor transfer logic required for while loop / case of where we relies on tensor reuse. Especially for while loop, we don't really do any data transfer at all (this is also understandable because if we do fresh allocation in while loop, memory will grow unbounded).

More over, debuggability concerns grow beyond just for this framework. It is now a concern for any frameworks for computation graphs. Here is my take: you pretty much need have a textual representation for any computation graph before debuggability comes into play. In this way, you can treat computation graph as imperative programming language, thus, step over, step into, rewind comes naturally. Inspecting variables in a scope, visualize it, inject some new values can also be beneficial. This is almost pointing to implement some form of Debug Adapter Protocol in VSCode and beyond. TensorBoard, on the other hand, doesn't make me feel is an adequate debugger, visualization, sure. Debugger requires two way communication which is not well-defined for TensorBoard with TF driver.


2019-05-03
----------
Have a rough implementation where for high level API such as ccv_cnnp_model, we can do forward pass, and then do backward pass separately.

This is helpful because we can customize losses (thinking about RL), accumulate gradients (useful for detection), and even use ccv_cnnp_model as a imperative part of a bigger model (i.e. using dynamic_graph to drive the computation, and use well-made ccv_cnnp_model for parts of it). I am very happy with where the abstraction goes.

However, the issue rises when I need to support outgrad in ccv_cnnp_model_backward. During backward, ingrad is provided (gradients corresponding to outputs). outgrad is not required, but if you provided, the gradients can flow over all the way to the input. In this way, ccv_cnnp_model can truly be part of a bigger model. This imposes a challenge though. To get the gradient, ccv_nnc_symbolic_graph_backward need to know which tensor we need to compute gradient against. The inputs are not provided in ccv_cnnp_model_evaluate / ccv_cnnp_model_fit's jitting. Thus, there is no such tensor symbol we can bind to as outgrad. This is relatively easy to resolve. We simply need to add these to the list of tensors requires gradients.

nnc's implementation optimizes both memory usage and computation aggressively. Thus, allocating additional memory and computation doesn't settle well. Alternatively, I can re-jit if outgrad provided, adding even more modes. Now, imagining we'd like to take some memory penalty for greater goods, thus, for multistage mode, we will generate a graph that computes the input gradient as well, is there a way for us to say, skip the computation penalty at least? Even this, unfortunately, doesn't seem obviously to me. For most ops, it is safe to pass that gradient in as 0, and it can skip. But for 1, it is not universal, we simply haven't enforced this and don't know if the outgrad is aggregated. Second, we cannot actually pass 0 after compiling symbolic graph to concrete one. The reason is because tensor can be unwrapped, therefore, we cannot simply assign a tensor to 0. Alternatively, safer option would be make tensor.data.u8 == 0, this is not ideal because either during command execution, we need to copy all tensor parameters out and make these tensors 0 if its underlying data.u8 is 0. Otherwise, in every single op implementation, we need to check both the tensor and its data.u8 for emptiness.

Probably complicating the interface more is a better solution (adding a 3rd parameter along requires_grad and is_test).


2019-05-01
----------
Start a worklog entry. Some of the thought process I had working on this project cannot be documented in the commit history. A worklog is a better place to write these down.
