# TinyWebServer

## 1. 概述

### 1.1 WebServer（网页服务器）

一个 Web Server 就是一个服务器软件（程序），或者是运行这个服务器软件的硬件（计算机）。其主要功能是通过 **HTTP 协议与客户端（通常是浏览器Browser）** 进行通信，接收、存储、处理来自客户端的 HTTP 请求，并对请求做出 HTTP 响应，返回给客户端其请求的内容（文件、网页等）或返回一个 Error 信息。

![image-20230906162927150](TinyWebServer.assets/image-20230906162927150.png)

### 1.2 服务器框架

虽然服务器程序种类繁多，但其基本框架都一样，不同之处在于逻辑处理。

 ![image-20230906163027550](TinyWebServer.assets/image-20230906163027550.png)

基本框架主要由I/O单元，逻辑单元和网络存储单元组成，其中每个单元之间通过**请求队列**进行通信，从而协同完成任务。

* **I/O 处理单元**

  I/O 处理单元是服务器管理客户连接的模块，通常要完成以下工作：等待并接受新的客户连接，接收客户数据，将服务器响应数据返回给客户端。但是数据的收发不一定在 I/O 处理单元中执行，也可能在逻辑单元中执行，具体在何处执行取决于**事件处理模式**。 

* **逻辑单元**

  **一个逻辑单元通常是一个进程或线程**，它分析并处理客户数据，然后将结果传递给 I/O 处理单元或者直接发送给客户端（具体使用哪种方式取决于事件处理模式）。服务器通常拥有多个逻辑单元，以实现对多个客户任务的并发处理。

* 网络存储单元

  网络存储单元可以是数据库、缓存和文件，但不是必须的。

* **请求队列**

  请求队列是各单元之间的通信方式的抽象。I/O  处理单元接收到客户请求时，需要以某种方式通知一个逻辑单元来处理该请求。同样，多个逻辑单元同时访问一个存储单元时，也需要采用某种机制来协调处理竞态条件。==请求队列通常被实现为池的一部分==。

### 1.3 事件处理模式

服务器程序通常需要处理三类事件：**I/O 事件、信号及定时事件**。

有两种高效的事件处理模式：

* **Reactor**  **同步 I/O 模型**通常用于实现 Reactor 模式。
* **Proactor** **异步 I/O 模型**通常用于实现 Proactor 模式。

#### 1.3.1 Reactor 模式

要求**主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生**，有的话就立即将该事件通知**工作线程（逻辑单元）**，将 socket **可读可写事件放入请求队列，交给工作线程处理**。除此之外，主线程不做任何其他实质性的工作。读写数据，接受新的连接，以及处理客户请求均在工作线程中完成。

使用同步 I/O（以 epoll_wait 为例）实现的 Reactor 模式的工作流程： 

* 主线程往  epoll 内核事件表中注册 socket 上的读就绪事件。
* 主线程调用 epoll_wait 等待 socket 上有数据可读。
* 当 socket  上有数据可读时， epoll_wait 通知主线程。**主线程则将 socket 可读事件放入请求队列**。 
* 睡眠在请求队列上的某个工作线程被唤醒，它从 socket 读取数据，并处理客户请求，然后**往 epoll 内核事件表中注册该 socket 上的写就绪事件**。
* 主线程调用 epoll_wait 等待 socket 可写。
* 当 socket  可写时，epoll_wait 通知主线程。**主线程将 socket 可写事件放入请求队列。**
* 睡眠在请求队列上的某个工作线程被唤醒，它往 socket 上写入服务器处理客户请求的结果。

Reactor 模式的工作流程：

![image-20230906164037604](TinyWebServer.assets/image-20230906164037604.png)

#### 1.3.2 Proactor 模式

**Proactor 模式将所有 I/O 操作都交给主线程和内核来处理（进行读、写），工作线程仅仅负责业务逻辑process。**

使用**异步 I/O 模型**（以 aio_read 和 aio_write 为例）实现的 Proactor 模式的工作流程： 

* 主线程调用 aio_read 函数向内核注册 socket 上的读完成事件，并告诉内核用户读缓冲区的位置， 以及读操作完成时如何通知应用程序（这里以信号为例）。     
* 主线程继续处理其他逻辑。
* 当 socket  上的数据被读入用户缓冲区后，内核将向应用程序发送一个信号，以通知应用程序数据已经可用。
* 应用程序预先定义好的==信号处理函数==选择一个工作线程来处理客户请求。工作线程处理完客户请求后，调用 aio_write 函数向内核注册 socket 上的写完成事件，并告诉内核用户写缓冲区的位置，以及写操作完成时如何通知应用程序。
* 主线程继续处理其他逻辑。 
* 当用户缓冲区的数据被写入  socket 之后，内核将向应用程序发送一个信号，以通知应用程序数据已经发送完毕。
* 应用程序预先定义好的信号处理函数选择一个工作线程来做善后处理，比如决定是否关闭 socket。

Proactor 模式的工作流程： 

![image-20230906164308698](TinyWebServer.assets/image-20230906164308698.png)

#### 1.3.3 模拟 Proactor 模式

由于异步I/O并不成熟，实际中使用较少，所以可以**使用同步 I/O 方式模拟出 Proactor 模式**。

原理是：**主线程执行数据读写操作，读写完成之后，主线程向工作线程通知这一完成事件**。那么从工作线程的角度来看，它们就直接获得了数据读写的结果，接下来要做的只是对读写的结果进行逻辑处理。 

使用同步 I/O 模型（以 epoll_wait为例）模拟出的 Proactor 模式的工作流程如下： 

* 主线程往 epoll 内核事件表中注册 socket 上的读就绪事件。 
* 主线程调用 epoll_wait等待 socket 上有数据可读。 
* 当 socket 上有数据可读时，epoll_wait 通知主线程。主线程从 socket 循环读取数据，直到没有更多数据可读，然后将读取到的数据封装成一个请求对象并插入请求队列。 
* 睡眠在请求队列上的某个工作线程被唤醒，它获得请求对象并处理客户请求，然后往 epoll 内核事件表中注册 socket 上的写就绪事件。 
* 主线程调用 epoll_wait 等待 socket 可写。 
* 当 socket 可写时，epoll_wait 通知主线程。主线程往 socket 上写入服务器处理客户请求的结果。

同步 I/O 模拟 Proactor 模式的工作流程：

![image-20230906164623477](TinyWebServer.assets/image-20230906164623477.png)

### 1.4 线程池

线程池是由服务器**预先创建的一组子线程，线程池中的线程数量应该和 CPU 数量差不多**。线程池中的所有子线程都运行着相同的代码。当有新的任务到来时，主线程将通过某种方式选择线程池中的某一个子线程来为之服务。相比与动态的创建子线程，选择一个已经存在的子线程的代价显然要小得多。至于主线程选择哪个子线程来为新任务服务，则有多种方式：

* 主线程使用某种算法来主动选择子线程。最简单、最常用的算法是随机算法和 Round Robin（轮流选取）算法，但更优秀、更智能的算法将**使任务在各个工作线程中更均匀地分配，从而减轻服务器的整体压力**。 
* ==主线程和所有子线程通过一个**共享的工作队列**来同步==，子线程都睡眠在该工作队列上。当有新的任务到来时，主线程将任务添加到工作队列中。这将==唤醒正在等待任务的子线程==，不过只有一个子线程将获得新任务的”接管权“，它可以从工作队列中取出任务并执行之，而其他子线程将继续睡眠在工作队列上。     

线程池的一般模型为：

![image-20230906164840082](TinyWebServer.assets/image-20230906164840082.png)

> 线程池中的线程数量最直接的限制因素是**中央处理器(CPU)的处理器(processors/cores)的数量 N** ：如果CPU是4-cores的，对于CPU密集型的任务(如视频剪辑等消耗CPU计算资源的任务)来说，那线程池中的线程数量最好也设置为4（或者+1防止其他因素造成的线程阻塞）；对于IO密集型的任务，一般要多于CPU的核数，因为线程间竞争的不是CPU的计算资源而是IO，IO的处理一般较慢，多于cores数的线程将为CPU争取更多的任务，不至在线程处理IO的过程造成CPU空闲导致资源浪费。 

**线程池的特点**

* ==**空间换时间**==，浪费服务器的硬件资源，换取运行效率。
* **池是一组资源的集合**，这组资源在服务器启动之初就被完全创建好并初始化，这称为==静态资源==。
* 当服务器进入正式运行阶段，开始处理客户请求的时候，如果它需要相关的资源，可以直接从池中获取，==无需动态分配==。
* 当服务器处理完一个客户连接后，可以把相关的资源放回池中，==无需执行系统调用释放资源==。

### 1.5 有限状态机

逻辑单元内部的一种==高效编程方法：**有限状态机（finite state machine）**==。

有的应用层协议头部包含数据包类型字段，每种类型可以映射为逻辑单元的一种**执行状态**，服务器可以根据它来编写相应的处理逻辑。如下是一种状态独立的有限状态机：

![image-20230906165054802](TinyWebServer.assets/image-20230906165054802.png)

这是一个简单的有限状态机，只不过该状态机的每个状态都是相互独立的，即状态之间没有相互转移。

状态之间的转移是需要状态机内部驱动，如下代码：

```c++
STATE_MACHINE() {
    State cur_State = type_A;
    while( cur_State != type_C ) {
        Package _pack = getNewPackage();
        switch( cur_State )
        {
            case type_A:
                process_package_state_A( _pack );
                cur_State = type_B;
                break;
            case type_B:
                process_package_state_B( _pack );
                cur_State = type_C;
                break;
        }
    }
}
```

该状态机包含三种状态：type_A、type_B 和 type_C，其中 type_A 是状态机的开始状态，type_C 是状态机的结束状态。状态机的当前状态记录在 cur_State 变量中。在一趟循环过程中，状态机先通过 getNewPackage 方法获得一个新的数据包，然后根据 cur_State 变量的值判断如何处理该数据包。数据包处理完之后，状态机通过给 cur_State 变量传递目标状态值来实现状态转移。那么当状态机进入下一 趟循环时，它将执行新的状态对应的逻辑。

### 1.6 RAII机制

RAII（Resource Acquisition Is Initialization）是由c++之父Bjarne Stroustrup提出的，中文翻译为==**资源获取即初始化**==，他说：==**使用局部对象来管理资源的技术称为资源获取即初始化**==；这里的资源主要是指操作系统中有限的东西如==内存、网络套接字==等等，局部对象是指存储在==栈的对象==，它的生命周期是由操作系统来管理的，无需人工介入；

* **RAII的原理**

  资源的使用一般经历三个步骤 a. 获取资源 b. 使用资源 c. 销毁资源，但是资源的销毁往往是程序员经常忘记的一个环节，所以程序界就想如何在程序员中让资源自动销毁呢？

  c++之父给出了解决问题的方案：RAII，它充分的==**利用了C++语言局部对象自动销毁的特性来控制资源的生命周期**==。给一个简单的例子来看下局部对象的自动销毁的特性：

  ```c++
  #include <iostream>
  #include <string>
  using namespace std;
  class person {
  public:
      person(const string name = "", int age = 0) :
      name_(name), age_(age) {
          cout << "Init a person!" << endl;
      }
      ~person() {
          cout << "destory a person" << endl;
      }
      const string& getname() const {
          return this->name_;
      }    
      int getage() const {
          return this->age_;
      }      
  private:
      const std::string name_;
      int age_;  
  };
  int main() {
      person p;
      return 0;
  }
  
  ```

  ![image-20230906175200769](TinyWebServer.assets/image-20230906175200769.png)

  从 person class 可以看出，在main函数中声明一个局部对象的时候，会==**自动调用构造函数进行对象的初始化**==，当整个main函数执行完成后，==**自动调用析构函数来销毁对象**==，整个过程无需人工介入，由操作系统自动完成；于是，很自然联想到，当我们在使用资源的时候，在构造函数中进行初始化，在析构函数中进行销毁。

  整个RAII过程我总结四个步骤：

  1. 设计一个类封装资源
  2. 在构造函数中初始化
  3. 在析构函数中执行销毁操作
  4. 使用时声明一个该对象的类

### 1.7 并发编程模式

并发模式是指==**IO处理单元**==和==**多个逻辑单元**==之间==**协调**==完成任务的方法。服务器主要有两种并发编程模式：半同步/半异步 、领导者/追随者模式

#### 1.7.1 半同步/半异步

> 在IO模型中，同步和异步指的是是内核向应用程序是何种IO事件(是就绪事件还是完成事件)，以及该有谁来完成IO读写(是应用程序还是内核)
>
> 在并发模式中，同步指的是程序完全==按照代码序列的顺序执行==；异步指的是程序的执行需要==由系统事件（比如中断、信号等）==的驱动

异步线程执行效率高，实时性强，但是难调试和难扩展；同步线程效率相对低，实时性差，但是逻辑简单容易编写。因此，对于像服务器这种即要求较好的实时性，有要求能够同时处理多个客户请求的应用程序，应该同时使用同步线程和异步线程实现，即采用**半同步/半异步**模式来实现。

##### 半同步/半异步模式

* ==同步线程用于处理客户逻辑==；==异步线程用于处理IO事件==。
* 异步线程监听到客户请求后，就将其封装成请求对象并插入请求队列中。
* 请求队列将通知某个工作在同步模式的工作线程读取并处理该请求对象。具体选择哪个工具来请求新的客户请求服务，则取决于请求队列的设计：
  * 轮流选取工作线程的Round Robin算法
  * 通过条件变量、信号量随机选择工作线程

在服务器程序中，如果结合考虑两种==**事件处理模式**==和==**几种IO模型**==，半同步/半异步模式存在很多变体：

##### 半同步/半反应堆模式

**半同步/半反应堆** 并发模式 是**半同步/半异步**的变体，将半异步具体化为==某种事件处理模式==，如procator或reactor。

* ==**异步线程只有一个，由主线程充当**==，负责监听所有socket上的事件：
  * 如果监听socket上有可读事件发生，即有新的连接请求到来，主线程就接受之，以得到新的连接socket，然后往epoll内核事件表中==注册该socket上的读写==事件。
  * 如果监听socket上有读写事件发生，即有新的客户请求或者有数据要发送到客户端，主线程就将该连接socket==插入请求队列==中
* 所有工作线程都睡眠在请求队列上，当有任务到来时，他们将通过竞争（比如申请互斥锁）获得任务的接管权。这种竞争机制使得只有空闲的工作线程才有机会来处理新任务。

![img](TinyWebServer.assets/20210104133748727.png)

上图中，主线程插入请求队列中的任务是就绪连接的socket，说明上图采用的事件处理模式是**Reactor模式**：它要求工作线程自己从socket上读取客户请求和往socket写入服务器应答。这就是该模式名称中的(`half-reactive`)的含义。

当然，半同步/半反应堆模式也可以使用模拟的Proactor事件处理模式，即==由主线程来完成数据的读写==。这时，主线程一般会将应用程序==数据，任务类型等信息封装为一个任务对象==，然后将其(或者指向该任务对象的一个指针)插入请求队列。工作线程从请求队列中取得任务对象知乎，就可以直接处理不需要进行读写操作了。

* 以Proactor模式为例的**半同步/半反应堆**工作流程：
  * ==主线程充当异步线程==，负责监听所有socket上的事件
  * 若有新请求到来，==主线程接收==之以得到新的连接socket，然后往epoll内核事件表中注册该socket上的读写事件
  * 如果连接socket上有读写事件发生，主线程从socket上接收数据，并将数据封装成请求对象插入到请求队列中
  * 所有==工作线程睡眠==在请求队列上，当有任务到来时，通过==竞争（如互斥锁）获得任务的接管权==

**半同步/半反应堆模式的缺点：**

* 主线程和工作线程共享请求队列。主线程往请求队列中加任务，或者工作线程从请求队列中取出任务，都需要对请求队列==加锁保护，从而白白耗费CPU时间==
* 每个工作线程在同一时间只能处理一个客户请求。如果客户数量较多，而工作线程较少，则请求队列中将堆积很多任务对象，客户端的响应速度将越来越慢。如果通过增加工作线程来解决这个问题，则工作线程的切换也将耗费大量的时间。

##### 高效的半同步/半异步模式

> 每个工作线程都能同时处理多个客户连接

主线程只管理监听socket，==连接socket由工作线程管理==。

* 当有新连接到来时，主线程就接受，并将新返回的连接socket派发给某个工作线程
  * 主线程向工作线程派发socket的最简单的方式，是往它和工作线程之间的管理里写数据。
* 主线程将连接socket派发完毕之后，该新socket上的任何IO操作都有被选中的工作线程来处理，直到客户关闭连接
  * **工作线程**检测到管道上有数据可读时，就分析是否是一个新的客户连接请求到来。如果是，则把该新socket上的读写事件注册到自己的epoll内核事件表中

![img](TinyWebServer.assets/20210104135307172.png)

每个线程（主线程和工作线程）都维持自己的事件循环，他们各自独立的监听不同的事件。每个线程都工作在异步模式，所以它并不是严格意义上的半同步/半异步模式。

#### 1.7.2 领导者/追随者模式

领导者/追随者模式是**多个工作线程轮流获得事件源集合，轮流监听、分发并处理事件的一种模式**。在任意时间点，程序都仅有一个领导者线程，它负责监听IO事件，而其他线程则都是追随者，它们休眠在线程池中等待成为新的领导者。当前的领导者如果监测到IO事件，则先从线程池中推选出新的领导者线程，然后处理IO事件。此时，新的领导者等待新的IO事件，而原来的领导者处理IO事件，实现了并发。

### 1.3 触发模式

* LT水平触发模

  epoll_wait 检测到文件描述符有事件发生，则将其通知给应用程序，应用程序可以不立即处理该事件。当下一次调用epoll_wait时，epoll_wait还会**再次向应用程序报告此事件，直至被处理**

* ET边缘触发模式

  epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序必须立即处理该事件，**必须要一次性将数据读取完，使用非阻塞I/O，读取到出现eagain**。

  即使可以使用 **ET 模式**，一个 socket 上的某个事件还是**可能被触发多次。**

  比如一个线程在读取完某个 socket 上的数据后开始处理这些数据，而在数据的处理过程中该 socket 上又有新数据可读（EPOLLIN 再次被触发），此时另外一个线程被唤醒来读取这些新的数据。于是就出现了**两个线程同时操作一个 socket 的局面**。但我们期望的是**一个socket连接在任一时刻都只被一个线程处理**， 通过**epoll_ctl对该文件描述符注册epolloneshot事件，一个线程处理socket时，其他线程将无法处理，当该线程处理完后，需要通过epoll_ctl重置epolloneshot事件。**

  对于注册了 ==**EPOLLONESHOT 事件**==的文件描述符，**操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次，除非我们使用 epoll_ctl 函数重置该文件描述符上注册的 EPOLLONESHOT 事 件**。这样，当一个线程在处理某个 socket 时，其他线程是不可能有机会操作该 socket 的。但反过来思考，注册了 EPOLLONESHOT 事件的 socket 一旦被某个线程处理完毕， 该线程就应该立即重置这个 socket 上的 EPOLLONESHOT 事件，以确保这个 socket 下一次可读时，其 EPOLLIN 事件能被触发，进 而让其他工作线程有机会继续处理这个 socket。 

## 2. web 服务器1（线程池）

> 实现**线程池**，采用非阻塞 socket 和 I/O多路复用的 epoll 工作模式（支持LT和ET） ，支持 Reactor 和 模拟Proactor 的事件处理模式，采用半同步/半反应堆并发模式。

当有客户端尝试连接web服务器上正在监听的端口，监听到的连接需要排队等待被accept。由于用户连接请求是随机到达的异步事件，每当监听套接字监听到新的客户连接到达后，epoll实例告知web服务器有连接来了，需要accpet这个连接，并分配一个逻辑单元（线程）来处理这个用户的请求，而且在处理这个请求的同时，需要继续监听其他客户的请求并分配另一个逻辑单元来处理，所以需要有多个线程，这里采用线程池来实现。服务器通过epoll这种I/O复用技术来实现对监听套接字和连接套接字的同时监听。

**pthread_create陷阱**

```c++
int pthread_create (pthread_t *__restrict __newthread,  // 返回新生成的线程的id
                    const pthread_attr_t *__restrict __attr, // 指向线程属性的指针, 通常设置为NULL
                    void *(*__start_routine) (void *), // 处理线程函数的地址
                    void *__restrict __arg) __THROWNL __nonnull ((1, 3)); //start_routine()中的参数
```

该函数的原型中第三个参数类型为函数指针，指向线程创建后运行的函数的地址。该函数要求为**静态函数**，所以如果该参数为类成员函数时，需要将其设置为==**静态成员函数**==。那为什么需要设置为静态函数呢，这是因为类成员函数自带this指针，会作为默认的参数被传入函数中，而==**线程处理函数指针的原型的参数是void*，与this的类型不匹配，所以不能通过编译**==，使用静态成员函数，没有this指针从而避免该问题。

#### 线程池实现

```c++
// 线程池类
template< typename T>
class threadpool {
public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:
    static void* worker(void * arg);
    void run();

private:
    int m_thread_number;                // 线程的数量
    pthread_t * m_threads;              // 描述线程池的数组，大小为 m_thread_number
    int m_max_requests;                 // 请求队列中最多允许等待处理的请求数量
    std::list<T*> m_workqueue;          // 请求队列
    locker m_queuelocker;               // 互斥锁
    sem m_queuestat;                    // 信号量，是否有任务需要处理
    connection_pool *m_connPool;        // 数据链接库
    int m_actor_model;                  // 事件处理模式
};
```

* 构造函数：创建m_thread_number个线程，为它们注册工作函数work，并设置detach可以自动回收资源

  ```c++
  template< typename T>
  threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) :
          m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests),
          m_connPool(connPool), m_threads(NULL) {
      if ((thread_number <= 0) || (max_requests <= 0)) {
          throw std::exception();
      }
  	// 创建m_thread_number大小的pthread_t数组，用来放创建好的线程
      m_threads = new pthread_t[m_thread_number];
      if (!m_threads) {
          throw std::exception();
      }
  
      // 调用pthread_create创建thread_number个线程，
      for (int i = 0; i < thread_number; ++i) {
          printf("create the %dth thread\n", i);
          // 创建线程，worker函数是创建的线程将要允许的函数，注意它必须是类的静态函数
          if (pthread_create(m_threads+i, NULL, worker, this ) != 0) {
              delete [] m_threads;
              throw std::exception();
          }
          // 将创建好的线程设置为 detach
          if (pthread_detach(m_threads[i])) {
              delete [] m_threads;
              throw std::exception();
          }
      }
  }
  ```

* 线程处理函数 work：工作线程创建后，就开始执行该函数

  ```c++
  // 工作线程运行的函数
  // 静态函数，没有this指针，但是可以通过传入的参数arg来访问线程池的对象
  template< typename T> void * threadpool<T>::worker(void * arg) {
      threadpool * pool = (threadpool *)arg;
      pool->run();
      return pool;
  }
  ```

* ==**任务执行函数 run**== ：实现工作线程从请求队列中取出某个任务进行处理，多线程之间会竞争任务，所以需要互斥锁，而任务又需要添加和消费，所以需要信号量（消费者，生产者）

  ```c++
  template< typename T> void threadpool<T>::run() {
      while (true) {
          // 信号量等待
          m_queuestat.wait();
          // 走到这里说明：被请求队列上的任务唤醒，则加互斥锁
          m_queuelocker.lock();
          // 走到这里说明：加锁成功
          if (m_workqueue.empty()) {
              // 虽然抢到了锁，但是任务已经被支持了，则解锁，再次等待被唤醒
              m_queuelocker.unlock();
              continue;
          }
          // 取出请求队列的第一个任务
          T* request = m_workqueue.front();
          m_workqueue.pop_front();
          // 解锁，让其他线程可以继续去任务执行
          m_queuelocker.unlock();
          if (!request) {
              continue;
          }
          // 执行任务
          if (m_actor_model == 1) {
              // reactor模式
              if (request->m_state == 0) {
                  // 工作线程需要进行读操作+处理操作
                  if (request->read()) {
                      // 读取完成，置improv为1，供主线程进行判断
                      request->improv = 1;
                      request->process();
                      connectionRAII mysqlcon(&request->mysql, m_connPool);// mysql连接
                  } else {
                      // 读取完成但失败，置improv为1，timer_flag为0，供主线程进行判断
                      request->improv = 1;
                      request->timer_flag = 1;
                  }
              } else {
                  // 工作线程需要进行写操作
                  if (request->write()) {
                      // 写入完成，置improv为1，供主线程进行判断
                      request->improv = 1;
                  } else {
                      // 写入完成但失败，置improv为1，timer_flag为0，供主线程进行判断
                      request->improv = 1;
                      request->timer_flag = 1;
                  }
              }
          } else {
              // reactor模式，工作线程只需要进行处理操作即可，数据已经由主线程放入http连接的缓冲区种了
              connectionRAII mysqlcon(&request->mysql, m_connPool);
              request->process();
          }
      }
  }
  ```

* 向请求队列中添加请求 append，append_p，一个包含读取操作加处理操作，一个只需要处理。这里也需要互斥锁来防止互斥资源的同时访问，也需要信号量的V操作，以唤醒因为P操作阻塞在请求队列上的工作线程。这两个函数由主线程调用，为工作线程添加任务。

  ```c++
  template< typename T> bool threadpool<T>::append(T* request, int state) {
      m_queuelocker.lock();
      // 如果请求队列已经超过最大请求数量，则解锁并返回false
      if (m_workqueue.size() >= m_max_requests) {
          m_queuelocker.unlock();
          return false;
      }
      // 工作线程需要进行读写操作，置标志位
      request->m_state = state;
      // 将请求插入工作队列
      m_workqueue.push_back(request);
      m_queuelocker.unlock();
      // 信号量+1，以通知工作线程有任务需要处理
      m_queuestat.post();
      return true;
  }
  // 与前一个函数功能类似，只不过这里添加的任务不需要工作线程进行读写操作
  template <typename T> bool threadpool<T>::append_p(T *request)
  {
      m_queuelocker.lock();
      if (m_workqueue.size() >= m_max_requests)
      {
          m_queuelocker.unlock();
          return false;
      }
      m_workqueue.push_back(request);
      m_queuelocker.unlock();
      m_queuestat.post();
      return true;
  }
  ```

#### 线程池分析

* 线程池的并发模式为：**半同步/半反应堆（procator和reactor)**

* 主线程为异步线程，负责监听文件描述符，接收socket新连接，若当前监听的socket发生了读写事件，然后将任务插入到请求队列。工作线程从请求队列中取出任务，完成读写数据的处理（HTTP请求报文的解析等等）。

  ```c++
  ...
  while (!stop_server) {
      int number = epoll_wait(m_epollfd, m_events, MAX_EVENT_NUMBER, -1);
      if (number < 0 && errno != EINTR) {
          LOG_ERROR("%s", "EPOLL failure");
          break;
      }
      
      for (int i = 0; i < number; ++i) {
          int sockfd = m_events[i].data.fd;
          if (sockfd == m_listenfd) {
              // 处理新到的客户连接
              bool flag = dealclinetdata();
              if (flag == false)
                  continue;
          } else if (m_events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR )) {
              // 2.6.7 版本内核中增加 EPOLLRDHUP 事件表示对端断开连接
              // 服务器端关闭连接，移除对应的定时器
              util_timer * timer = m_users_timer[sockfd].timer;
              deal_timer(timer, sockfd);
          } else if ((sockfd == m_pipefd[0]) && (m_events[i].events & EPOLLIN)) {
              // 信号相关处理
              ...
          } else if (m_events[i].events & EPOLLIN) {
              // sockedfd上有读事件发生
              dealwithread(sockfd);
          } else if (m_events[i].events & EPOLLOUT) {
              // sockedfd上有写事件发生
              dealwithwrite(sockfd);
          }
      }
      ...
  }
  ...
  ```

* * **主线程通过epoll可以知道有新连接达到**（监听套接字listenfd就绪），使用accept()接收，并返回一个新的socket文件描述符 connfd 用于和用户的通信，并将这个connfd注册到epoll实例中，等用户发来请求报文。

    ```c++
    bool WebServer::dealclinetdata() {
        // 处理新的客户端连接请求
        struct sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);
        if (m_LISTENTrigmode == 0)
        {   // LT 水平触发模式下，不强迫一次性接受数据
            int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlen);
            ...
            // 将connfd加入epoll监听事件
        } else { // ET 边缘触发模式下，需要用while循环读取到所有数据才可以，因为会有多个请求到达连接缓冲区，而边缘触发模式下只会触发一次读事件，因此必须在收到连接读事件后用处理所有连接
            while (1) {
                int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                ...
                // 将connfd加入epoll监听事件
            }
            return false;
        }
        return true;
    }
    ```

  * 之后的某一时刻通过epoll 发现这个connfd上有可读事件了（EPOLLIN）

    * 在procator模式下，主线程就将这个HTTP的请求报文**读进这个连接socket的读缓存中**users[sockfd].read()，然后将该任务对象（指针）插入线程池的请求队列中 **pool->append_p(users  + sockfd)**，唤醒某个工作线程来进行处理
    * 在reactor模式下，主线程直接将该任务对象插入线程池队列种 **pool->append(users  + sockfd, 0)**，唤醒某个工作线程来读取数据并处理。

    ```C++
    void WebServer::dealwithread(int sockfd) {
        ...
        if (m_actormodel == 1) {
            // reatcor模式，在工作线程进行数据读取
            // 将事件放入请求队列，以唤醒某个工作线程读取sockfd上的数据并处理
            m_pool->append(m_http_users + sockfd, 0);
            while (true) {
                if (m_http_users[sockfd].improv == 1) {
                    // 工作线程读操作完成
                    if (m_http_users[sockfd].timer_flag == 1) {
                        // 工作线程读操作出错
                        deal_timer(timer, sockfd);
                        m_http_users[sockfd].timer_flag = 0; // 重置工作线程读取出错flag
                    }
                    // 重置工作线程读取完成标志
                    m_http_users[sockfd].improv = 0;
                    break;
                } 
            }
        } else {
            // proactor模式，直接在主线程进行数据读取
            if (m_http_users[sockfd].read()) {
                // 主线程读取成功后，将事件放入请求队列，唤醒某个工作线程进行处理
                m_pool->append_p(m_http_users + sockfd);
                ...
            } else {
                // 主线程读取失败
                deal_timer(timer, sockfd);
            }
        }
    }
    
    void WebServer::dealwithwrite(int sockfd) {
    	...
        if (m_actormodel == 1) {
            // proatcor模式，在工作线程进行数据写入
            // 将该事件放入请求队列，以唤醒某个工作线程写入sockfd
            m_pool->append(m_http_users + sockfd, 1);
            while (true) {
                if (m_http_users[sockfd].improv == 1) {
                    // 工作线程写操作完成
                    if (m_http_users[sockfd].timer_flag == 1) {
                        // 工作线程写操作出错
                        deal_timer(timer, sockfd);
                        m_http_users[sockfd].timer_flag = 0; // 重置工作线程读取出错flag
                    }
                    // 重置工作线程读取完成标志
                    m_http_users[sockfd].improv = 0;
                    break;
                } 
            }
        } else {
            // proactor模式，直接在主线程进行数据写入
            if (m_http_users[sockfd].write()) {
                // 主线程写入成功
                ...
            } else {
                // 主线程写入失败
                deal_timer(timer, sockfd);
            }
        }
    }
    ```

## 3. web服务器2（HTTP连接）

#### http处理流程

![](TinyWebServer.assets/http.jpg)

* **连接处理**：浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
* **处理报文请求**：工作线程取出任务后，调用process_read函数，通过主、从状态机对请求报文进行解析。
* **返回响应报文**：解析完之后，跳转do_request函数生成响应报文，通过process_write写入buffer，返回给浏览器端。

#### http类

在http类包含了很多内容

* 初始化，当主线程通过epoll知道有新连接达到后，进行http对象的初始化操作

  ```c++
  void init(int sockfd, const sockaddr_in & addr, char *root, 
            int TRIGMode, int close_log, string user, string passwd, string sqlname);
  int sockfd; // 该HTTP连接的socket和对方的socket地址
  sockaddr_in addr; // 对方的socket地址
  char * root; // 网站的根目录
  int TRIGMode; // 监听和连接fd的触发模式
  int close_log; // 是否关闭日志
  string user; // 登陆数据库用户名
  string password;  // 登陆数据库密码
  string databasename; // 使用数据库名
  ```

* read 函数，读取sockfd中的数据到http对象的m_read_buf缓冲区中，有ET模式和LT模式

  ```c++
  bool http_conn::read() {
      if (m_read_idx >= READ_BUFFER_SIZE) {
          return false;
      }
      // 已经读取到的字节
      int bytes_read = 0;
      // LT读取数据
      if (m_TRIGMode == 0) {
          bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
          m_read_idx += bytes_read;
          if (bytes_read <= 0) {
              return false;
          }
          return true;
      } else { // ET读数据
          while (true) {
              // 每次缓冲区处于可读状态时就触发epoll，此时包可能还没有都过来，所以需要用while全部接受
              // 但是我在想如果一个http包特别大，把整个接受缓冲区都沾满了但是还没有读完，也没有人来处理缓冲区的内容，遮该怎么办，可能使用Buffer
              bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
              if (bytes_read == -1) {
                  if (errno == EAGAIN || errno == EWOULDBLOCK) {
                      // 读取完毕
                      break;
                  } else {
                      return false;
                  }
              } else if (bytes_read == 0) {
                  // 对方关闭连接
                  return false;
              }
              m_read_idx += bytes_read;
          }
          //printf("%s", m_read_buf);
          return true;
      }
  }
  ```
