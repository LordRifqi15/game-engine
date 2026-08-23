# 📁 Project Structure (C++ + Vulkan Engine)

## 🧱 Root Structure

``` id="p1s8fj"
engine/
├── CMakeLists.txt
├── README.md
├── docs/
├── third_party/
├── assets/
├── shaders/
├── src/
├── include/
├── tests/
└── tools/
```

---

## 📂 `/src` (Implementation)

``` id="j4k2lm"
src/
├── core/
├── ecs/
├── renderer/
├── platform/
├── editor/
├── modules/
└── main.cpp
```

---

## 📂 `/include` (Headers)

``` id="b7n3qp"
include/
├── core/
├── ecs/
├── renderer/
├── platform/
├── editor/
└── modules/
```

---

## 🧠 Core Systems

### `/core`
- Application
- Logger
- Config
- Event system

---

### `/ecs`
- Entity
- Component
- System
- Registry

---

## 🎮 Rendering System (Vulkan)

### `/renderer`

``` id="x9c4vd"
renderer/
├── api/                # abstraction layer
├── vulkan/             # Vulkan backend
├── resources/          # meshes, textures
├── scene/              # camera, lights
└── renderer.cpp
```

---

### `/renderer/api`
- RenderDevice (interface)
- CommandBuffer (abstract)
- Pipeline abstraction

---

### `/renderer/vulkan`

``` id="m2q8we"
vulkan/
├── vk_instance.cpp
├── vk_device.cpp
├── vk_swapchain.cpp
├── vk_pipeline.cpp
├── vk_command_buffer.cpp
├── vk_renderpass.cpp
└── vk_sync.cpp
```

---

## 🧩 Modules System

``` id="r5t9yu"
modules/
├── physics/
├── audio/
├── ai/
├── networking/
```

Each module:
- Self-contained
- Register to engine

---

## 🖥️ Platform Layer

``` id="z8h6pl"
platform/
├── window/
├── input/
├── time/
```

Use:
- GLFW (start)
- Later custom abstraction

---

## 🎨 Editor

``` id="c3v7na"
editor/
├── ui/
├── panels/
├── viewport/
```

Uses:
- ImGui (initially)

---

## 🧪 Tests

``` id="t4k1bs"
tests/
├── ecs_tests.cpp
├── renderer_tests.cpp
```

---

## 🔧 Tools

``` id="w9e2dc"
tools/
├── asset_pipeline/
├── shader_compiler/
```

---

## 🎨 Shaders

``` id="g6n8fa"
shaders/
├── basic.vert
├── basic.frag
```

---

## 📦 Third Party

``` id="y2u5xo"
third_party/
├── glfw/
├── glm/
├── imgui/
├── volk/ (Vulkan loader)
```

---

## ⚡ Key Rules

### 1. NEVER expose Vulkan directly to game logic
Only access via:
```
Render API → Vulkan backend
```

---

### 2. Keep modules isolated
- No tight coupling
- Communicate via interfaces

---

### 3. Build small systems first
- Triangle → Mesh → Scene

---

## 🧭 First Milestone

Your AI coding buddy should build:

- Window (GLFW)
- Vulkan instance
- Swapchain
- Draw triangle

---

## 🔥 Philosophy

> Structure early = speed later  
> Abstraction early = flexibility later