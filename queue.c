
#include "driver.h"


//IO派遣的配置？
//只有一个单的、缺省的queue，缺点是串行
//context是谁分配的？未见明显代码
//queue的表示锁是什么东西？和queue什么关系？
//context的生命周期是什么？为什么绑定到Queue上？
//Queue是一个对象
//我们为什么要注册一个可选的析构函数？release any private allocation or resource
NTSTATUS
EchoQueueInitialize( //在EchoDeviceCreate中被调用
    WDFDEVICE Device
    )
/*++
     A single default I/O Queue is configured for serial request
     processing, and a driver context memory allocation is created
     to hold our structure QUEUE_CONTEXT.

     This memory may be used by the driver automatically synchronized
     by the Queue's presentation lock.

     The lifetime of this memory is tied to the lifetime of the I/O
     Queue object, and we register an optional destructor callback
     to release any private allocations, and/or resources.
--*/
{
    WDFQUEUE queue; //句柄
    NTSTATUS status;
    PQUEUE_CONTEXT queueContext; //驱动自定义的
    WDF_IO_QUEUE_CONFIG    queueConfig;
    WDF_OBJECT_ATTRIBUTES  queueAttributes;

    // configure-fowarded request是什么东西？
    // Configure a default queue so that requests that are not
    // configure-fowarded using WdfDeviceConfigureRequestDispatching to goto
    // other queues get dispatched here.
    //
	//第一部：初始化WDF_IO_QUEUE_CONFIG
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
           &queueConfig, //被初始化
            WdfIoQueueDispatchSequential //枚举
        );

	//继续初始化queueConfig，装两个必要的回调
    queueConfig.EvtIoRead   = EchoEvtIoRead;
    queueConfig.EvtIoWrite  = EchoEvtIoWrite;

	//问题：现在queueConfig有什么？

	//第二部：初始化WDF_OBJECT_ATTRIBUTES

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, QUEUE_CONTEXT);

    // 术语：同步范围（synchronization scope）是什么？
    // Set synchronization scope on queue and have the timer to use queue as
    // the parent object so that queue and timer callbacks are synchronized
    // with the same lock.这段话很难理解
    //
    queueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue; //枚举，同步范围到queue对象上？而queue有个timer儿子？
    
    //注册一个回调用于queue context的生命周期管理，注意：不是queue context本身，而是queue context里面再分配的内存什么的
    queueAttributes.EvtDestroyCallback = EchoEvtIoQueueContextDestroy;

	//第三部：创建queue对象
    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig, //上面初始化的
                 &queueAttributes, //上面初始化的
                 &queue //输出，句柄
                 );

    if( !NT_SUCCESS(status) ) {
        //...
        return status;
    }

	//要点：必须先创建queue对象，然后才能获得queue的context（内存）！
	//关键是用QUEUE_CONTEXT初始化了queueAttributes
	//所以：WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, QUEUE_CONTEXT)及其背后的机制很了不起

    // Get our Driver Context memory from the returned Queue handle

	//驱动自定义的，首次在此初始化
	
    queueContext = QueueGetContext(queue);

	//该干么干么
    queueContext->WriteMemory = NULL;
    queueContext->Timer = NULL; //下面马上创建

	//学习
    queueContext->CurrentRequest = NULL; //和cancel竞争有关
    queueContext->CurrentStatus = STATUS_INVALID_DEVICE_REQUEST;

    //
    // Create the Queue timer
    //
    status = EchoTimerCreate(&queueContext->Timer, queue);
    //...

    return status;
}


//子程序
//这里的timer对象和queue对象是什么关系？父子关系，queue是父，timer是子
//父子关系有什么好处：告诉frame说，把timer回调和queue回调串行化处理，不需要（在多线程条件下）保护queue contex了
NTSTATUS
EchoTimerCreate(  //不是微软的架构
    IN WDFTIMER*       Timer,
    IN WDFQUEUE        Queue
    )
/*++

--*/
{
    NTSTATUS Status;

	//也是要初始化两个东西，一个关于配置的，另一个关于对象的，都与创建有关

    WDF_TIMER_CONFIG       timerConfig; //frame的结构
    WDF_OBJECT_ATTRIBUTES  timerAttributes;

    //
    // Create a WDFTIMER object
    //
    WDF_TIMER_CONFIG_INIT(&timerConfig, EchoEvtTimerFunc);

    //
    // WDF_OBJECT_ATTRIBUTES_INIT sets AutomaticSerialization to TRUE by default
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    
    //下面这句话有深刻的含义，值得反复看
    timerAttributes.ParentObject = Queue; // Synchronize with the I/O Queue
    timerAttributes.ExecutionLevel = WdfExecutionLevelPassive; //枚举，很低的IRQL级别

    // 下面这句不明白
    // Create a non-periodic timer since WDF does not allow periodic timer 
    // with autosynchronization at passive level
    //
    Status = WdfTimerCreate(&timerConfig,
                             &timerAttributes,
                             Timer     // Output handle
                             );

    return Status;
}



VOID
EchoEvtIoQueueContextDestroy(
    WDFOBJECT Object
)
/*++
    This is called when the Queue that our driver context memory
    is associated with is destroyed.
--*/
{
    PQUEUE_CONTEXT queueContext = QueueGetContext(Object);

    //
    // Release any resources pointed to in the queue context.
    //
    // The body of the queue context will be released after
    // this callback handler returns
    //

    //
    // If Queue context has an I/O buffer, release it
    //
    if( queueContext->WriteMemory != NULL ) {
        WdfObjectDelete(queueContext->WriteMemory); //只删除queue下面的，queue本身
        queueContext->WriteMemory = NULL;
    }

    return;
}

//问题：我们什么时候选择了 use frameworks Device level locking？
VOID
EchoEvtRequestCancel(
    IN WDFREQUEST Request
    )
/*++
    Called when an I/O request is cancelled after the driver has marked
    the request cancellable. This callback is automatically synchronized
    with the I/O callbacks since we have chosen to use frameworks Device
    level locking.
--*/
{
    PQUEUE_CONTEXT queueContext = QueueGetContext(WdfRequestGetIoQueue(Request));

    //...

    //
    // The following is race free by the callside or DPC side
    // synchronizing completion by calling
    // WdfRequestMarkCancelable(Queue, Request, FALSE) before
    // completion and not calling WdfRequestComplete if the
    // return status == STATUS_CANCELLED.
    //
    WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0L);

    // 下句什么意思？
    // This book keeping记账 is synchronized by the common
    // Queue presentation lock
    //
    ASSERT(queueContext->CurrentRequest == Request);
    queueContext->CurrentRequest = NULL; //注意在完成函数之后，确定已经完成才这么干

    return;
}

//什么时候被调用：when the framework receives IRP_MJ_READ request.
//方向：context buffer ---->>--->>----request buffer
//技巧：可以推迟完成到别的地方，比如timer
VOID
EchoEvtIoRead(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t      Length
    )
/*++

Routine Description:

    This event is called when the framework receives IRP_MJ_READ request.
    It will copy the content from the queue-context buffer to the request buffer.
    If the driver hasn't received any write request earlier, the read returns zero.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Length  - number of bytes to be read.  //永远不会为0的原因要知道
              The default property of the queue is to not dispatch
              zero lenght read & write requests to the driver and
              complete is with status success. So we will never get
              a zero length request.
--*/
{
    NTSTATUS Status;
    PQUEUE_CONTEXT queueContext = QueueGetContext(Queue);
    WDFMEMORY memory; //句柄
    size_t writeMemoryLength;

    //...
    //...
             Queue,Request,Length));
    //
    // No data to read，如果句柄不在的话
    //
    if( (queueContext->WriteMemory == NULL)  ) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, (ULONG_PTR)0L);//直接完成，这恐怕是最简单的情况
        return;
    }

    //
    // Read what we have
    //
    WdfMemoryGetBuffer(queueContext->WriteMemory, &writeMemoryLength);
    _Analysis_assume_(writeMemoryLength > 0);

	//看看request能不能放得下
    if( writeMemoryLength < Length ) {
        Length = writeMemoryLength;
    }

    //
    // Get the request memory
    // 取得request上的内存，这个不会疯跑吧？
    Status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(Status) ) {
        //...
        WdfVerifierDbgBreakPoint();
        WdfRequestCompleteWithInformation(Request, Status, 0L);//错误也要完成
        return;
    }

    // Copy the memory out
    Status = WdfMemoryCopyFromBuffer( memory, // destination
                             0,      // offset into the destination memory
                             WdfMemoryGetBuffer(queueContext->WriteMemory, NULL),//用使用句柄内存就得用这个函数得到内存的地址
                             Length );
    if( !NT_SUCCESS(Status) ) {
        //...
        WdfRequestComplete(Request, Status);//错误也要完成
        return;
    }

    // Set transfer information
    WdfRequestSetInformation(Request, (ULONG_PTR)Length);

    // Mark the request is cancelable
	//这个时候标记可取消？时机是否得当？
    WdfRequestMarkCancelable(Request, EchoEvtRequestCancel);


    // Defer the completion to another thread from the timer dpc
    queueContext->CurrentRequest = Request; //如此次Defer
    queueContext->CurrentStatus  = Status;//Status不用修改？

    //没有调用WdfRequestComplete，显然没有完成

    return;
}


//什么时候被调用：when the framework receives IRP_MJ_WRITE request.
//方向：context buffer ----<<---<<----request buffer
//技巧：可以推迟完成到别的地方，比如timer
VOID
EchoEvtIoWrite(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t     Length
    )
/*++

Routine Description:

    This event is invoked when the framework receives IRP_MJ_WRITE request.
    This routine allocates memory buffer, copies the data from the request to it,
    and stores the buffer pointer in the queue-context with the length variable
    representing the buffers length. The actual completion of the request
    is defered to the periodic timer dpc.
--*/
{
    NTSTATUS Status;
    WDFMEMORY memory;
    PQUEUE_CONTEXT queueContext = QueueGetContext(Queue);
    PVOID writeBuffer = NULL;

    _Analysis_assume_(Length > 0);

    //...

    if( Length > MAX_WRITE_LENGTH ) {
        //...
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_OVERFLOW, 0L);//错误也要完成
        return;
    }

    // Get the memory buffer
    Status = WdfRequestRetrieveInputMemory(Request, &memory);//数据在request里面，这是源
    if( !NT_SUCCESS(Status) ) {
        //...
        WdfVerifierDbgBreakPoint();
        WdfRequestComplete(Request, Status);//错误也要完成
        return;
    }

    // Release previous buffer if set
    if( queueContext->WriteMemory != NULL ) {
        WdfObjectDelete(queueContext->WriteMemory);//这叫Release，难道不是delete？
        queueContext->WriteMemory = NULL;
    }

    Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, //每次都重新重建一块内存？
                             NonPagedPoolNx,
                             'sam1',//通常不超过4个char
                             Length,
                             &queueContext->WriteMemory, //句柄
                             &writeBuffer
                             );

    if(!NT_SUCCESS(Status)) {
        //...
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }


    // Copy the memory in
    Status = WdfMemoryCopyToBuffer( memory, //源头，来自request
                                    0,  // offset into the source memory
                                    writeBuffer,
                                    Length );
    if( !NT_SUCCESS(Status) ) {
        //...
        WdfVerifierDbgBreakPoint();

        WdfObjectDelete(queueContext->WriteMemory);
        queueContext->WriteMemory = NULL;

        WdfRequestComplete(Request, Status);
        return;
    }

    // Set transfer information
    WdfRequestSetInformation(Request, (ULONG_PTR)Length);

    // Specify the request is cancelable
    WdfRequestMarkCancelable(Request, EchoEvtRequestCancel);

    // Defer the completion to another thread from the timer dpc
    queueContext->CurrentRequest = Request;
    queueContext->CurrentStatus  = Status;

    //没有调用WdfRequestComplete，显然没有完成

    return;
}

//想不到在..INIT宏中被设置到timer中，还未有timer就被设置了
//作用是：完成请求
//会和queue的回调函数以及cancel过程发生同步
VOID
EchoEvtTimerFunc(
    IN WDFTIMER     Timer
    )
/*++

Routine Description:

    This is the TimerDPC the driver sets up to complete requests.
    This function is registered when the WDFTIMER object is created, and
    will automatically synchronize with the I/O Queue callbacks
    and cancel routine.

Arguments:

    Timer - Handle to a framework Timer object.

--*/
{
    NTSTATUS      Status;
    WDFREQUEST     Request;
    WDFQUEUE queue;
    PQUEUE_CONTEXT queueContext ;

    queue = WdfTimerGetParentObject(Timer);
    queueContext = QueueGetContext(queue);

    //
    // DPC is automatically synchronized to the Queue lock,
    // so this is race free without explicit driver managed locking.
    //
    Request = queueContext->CurrentRequest;
    if( Request != NULL ) { //目的是保证Request句柄还有效，如果cancel已经完成了，那么Request将在某点无效，就不能调用下面的WdfRequestUnmarkCancelable函数了
	
	//Therefore, your driver must not call XXXX after its EvtRequestCancel callback function has called WdfRequestComplete.

        //
        // Attempt to remove cancel status from the request.
        //
        // The request is not completed if it is already cancelled
        // since the EchoEvtIoCancel function has run, or is about to run
        // and we are racing with it.
        //
	//我们在和cancel函数在竞争...必须分出高下，靠下面的函数
        Status = WdfRequestUnmarkCancelable(Request);
        if( Status != STATUS_CANCELLED ) {

            queueContext->CurrentRequest = NULL; //竞争赢了！先标记
            Status = queueContext->CurrentStatus;

            //...

            WdfRequestComplete(Request, Status); //终于该完成了，大多数都是从这个地方完成的
        }
        else {
            //...
        }
    }

    //
    // Restart the Timer since WDF does not allow periodic timer 
    // with autosynchronization at passive level
    //
    WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(TIMER_PERIOD));

    return;
}


