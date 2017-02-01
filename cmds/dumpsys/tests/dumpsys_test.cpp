/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../dumpsys.h"

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android-base/file.h>
#include <utils/String16.h>
#include <utils/Vector.h>

using namespace android;

using ::testing::_;
using ::testing::Action;
using ::testing::ActionInterface;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::MakeAction;
using ::testing::Not;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Test;
using ::testing::WithArg;
using ::testing::internal::CaptureStderr;
using ::testing::internal::CaptureStdout;
using ::testing::internal::GetCapturedStderr;
using ::testing::internal::GetCapturedStdout;

using android::hardware::hidl_vec;
using android::hardware::hidl_string;
using android::hardware::Void;
using HServiceManager = android::hidl::manager::V1_0::IServiceManager;
using IServiceNotification = android::hidl::manager::V1_0::IServiceNotification;

class ServiceManagerMock : public IServiceManager {
  public:
    MOCK_CONST_METHOD1(getService, sp<IBinder>(const String16&));
    MOCK_CONST_METHOD1(checkService, sp<IBinder>(const String16&));
    MOCK_METHOD3(addService, status_t(const String16&, const sp<IBinder>&, bool));
    MOCK_METHOD0(listServices, Vector<String16>());

  protected:
    MOCK_METHOD0(onAsBinder, IBinder*());
};

class HardwareServiceManagerMock : public HServiceManager {
  public:
    template<typename T>
    using R = android::hardware::Return<T>; // conflicts with ::testing::Return

    MOCK_METHOD2(get, R<sp<IBase>>(const hidl_string&, const hidl_string&));
    MOCK_METHOD3(add,
        R<bool>(const hidl_vec<hidl_string>&,
                const hidl_string&,
                const sp<IBase>&));
    MOCK_METHOD1(list, R<void>(list_cb));
    MOCK_METHOD2(listByInterface,
        R<void>(const hidl_string&, listByInterface_cb));
    MOCK_METHOD3(registerForNotifications,
        R<bool>(const hidl_string&,
                const hidl_string&,
                const sp<IServiceNotification>&));
    MOCK_METHOD1(debugDump, R<void>(debugDump_cb));

};

class BinderMock : public BBinder {
  public:
    BinderMock() {
    }

    MOCK_METHOD2(dump, status_t(int, const Vector<String16>&));
};

// gmock black magic to provide a WithArg<0>(WriteOnFd(output)) matcher
typedef void WriteOnFdFunction(int);

class WriteOnFdAction : public ActionInterface<WriteOnFdFunction> {
  public:
    explicit WriteOnFdAction(const std::string& output) : output_(output) {
    }
    virtual Result Perform(const ArgumentTuple& args) {
        int fd = ::std::tr1::get<0>(args);
        android::base::WriteStringToFd(output_, fd);
    }

  private:
    std::string output_;
};

// Matcher used to emulate dump() by writing on its file descriptor.
Action<WriteOnFdFunction> WriteOnFd(const std::string& output) {
    return MakeAction(new WriteOnFdAction(output));
}

// gmock black magic to provide a WithArg<0>(List(services)) matcher
typedef void HardwareListFunction(HServiceManager::list_cb);

class HardwareListAction : public ActionInterface<HardwareListFunction> {
  public:
    explicit HardwareListAction(const hidl_vec<hidl_string> &services) : services_(services) {
    }
    virtual Result Perform(const ArgumentTuple& args) {
        auto cb = ::std::tr1::get<0>(args);
        cb(services_);
    }

  private:
    hidl_vec<hidl_string> services_;
};

Action<HardwareListFunction> HardwareList(const  hidl_vec<hidl_string> &services) {
    return MakeAction(new HardwareListAction(services));
}

// Matcher for args using Android's Vector<String16> format
// TODO: move it to some common testing library
MATCHER_P(AndroidElementsAre, expected, "") {
    std::ostringstream errors;
    if (arg.size() != expected.size()) {
        errors << " sizes do not match (expected " << expected.size() << ", got " << arg.size()
               << ")\n";
    }
    int i = 0;
    std::ostringstream actual_stream, expected_stream;
    for (String16 actual : arg) {
        std::string actual_str = String16::std_string(actual);
        std::string expected_str = expected[i];
        actual_stream << "'" << actual_str << "' ";
        expected_stream << "'" << expected_str << "' ";
        if (actual_str != expected_str) {
            errors << " element mismatch at index " << i << "\n";
        }
        i++;
    }

    if (!errors.str().empty()) {
        errors << "\nExpected args: " << expected_stream.str()
               << "\nActual args: " << actual_stream.str();
        *result_listener << errors.str();
        return false;
    }
    return true;
}

// Custom action to sleep for timeout seconds
ACTION_P(Sleep, timeout) {
    sleep(timeout);
}

class DumpsysTest : public Test {
  public:
    DumpsysTest() : sm_(), hm_(), dump_(&sm_, &hm_), stdout_(), stderr_() {
    }

    void ExpectListServices(std::vector<std::string> services) {
        Vector<String16> services16;
        for (auto& service : services) {
            services16.add(String16(service.c_str()));
        }

        EXPECT_CALL(sm_, listServices()).WillRepeatedly(Return(services16));
    }

    void ExpectListHardwareServices(std::vector<std::string> services) {
        hidl_vec<hidl_string> hidl_services;
        hidl_services.resize(services.size());
        for (size_t i = 0; i < services.size(); i++) {
            hidl_services[i] = services[i];
        }

        EXPECT_CALL(hm_, list(_)).WillRepeatedly(DoAll(
                WithArg<0>(HardwareList(hidl_services)),
                Return(Void())));
    }

    sp<BinderMock> ExpectCheckService(const char* name, bool running = true) {
        sp<BinderMock> binder_mock;
        if (running) {
            binder_mock = new BinderMock;
        }
        EXPECT_CALL(sm_, checkService(String16(name))).WillRepeatedly(Return(binder_mock));
        return binder_mock;
    }

    void ExpectDump(const char* name, const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, _))
            .WillRepeatedly(DoAll(WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void ExpectDumpWithArgs(const char* name, std::vector<std::string> args,
                            const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, AndroidElementsAre(args)))
            .WillRepeatedly(DoAll(WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void ExpectDumpAndHang(const char* name, int timeout_s, const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, _))
            .WillRepeatedly(DoAll(Sleep(timeout_s), WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void CallMain(const std::vector<std::string>& args) {
        const char* argv[1024] = {"/some/virtual/dir/dumpsys"};
        int argc = (int)args.size() + 1;
        int i = 1;
        for (const std::string& arg : args) {
            argv[i++] = arg.c_str();
        }
        CaptureStdout();
        CaptureStderr();
        int status = dump_.main(argc, const_cast<char**>(argv));
        stdout_ = GetCapturedStdout();
        stderr_ = GetCapturedStderr();
        EXPECT_THAT(status, Eq(0));
    }

    void AssertRunningServices(const std::vector<std::string>& services,
                               const std::string &message = "Currently running services:") {
        std::string expected(message);
        expected.append("\n");
        for (const std::string& service : services) {
            expected.append("  ").append(service).append("\n");
        }
        EXPECT_THAT(stdout_, HasSubstr(expected));
    }

    void AssertOutput(const std::string& expected) {
        EXPECT_THAT(stdout_, StrEq(expected));
    }

    void AssertOutputContains(const std::string& expected) {
        EXPECT_THAT(stdout_, HasSubstr(expected));
    }

    void AssertDumped(const std::string& service, const std::string& dump) {
        EXPECT_THAT(stdout_, HasSubstr("DUMP OF SERVICE " + service + ":\n" + dump));
    }

    void AssertNotDumped(const std::string& dump) {
        EXPECT_THAT(stdout_, Not(HasSubstr(dump)));
    }

    void AssertStopped(const std::string& service) {
        EXPECT_THAT(stderr_, HasSubstr("Can't find service: " + service + "\n"));
    }

    ServiceManagerMock sm_;
    HardwareServiceManagerMock hm_;
    Dumpsys dump_;

  private:
    std::string stdout_, stderr_;
};

TEST_F(DumpsysTest, ListHwServices) {
    ExpectListHardwareServices({"Locksmith", "Valet"});

    CallMain({"--hw"});

    AssertRunningServices({"Locksmith", "Valet"}, "Currently running hardware services:");
}

// Tests 'dumpsys -l' when all services are running
TEST_F(DumpsysTest, ListAllServices) {
    ExpectListServices({"Locksmith", "Valet"});
    ExpectCheckService("Locksmith");
    ExpectCheckService("Valet");

    CallMain({"-l"});

    AssertRunningServices({"Locksmith", "Valet"});
}

// Tests 'dumpsys -l' when a service is not running
TEST_F(DumpsysTest, ListRunningServices) {
    ExpectListServices({"Locksmith", "Valet"});
    ExpectCheckService("Locksmith");
    ExpectCheckService("Valet", false);

    CallMain({"-l"});

    AssertRunningServices({"Locksmith"});
    AssertNotDumped({"Valet"});
}

// Tests 'dumpsys service_name' on a service is running
TEST_F(DumpsysTest, DumpRunningService) {
    ExpectDump("Valet", "Here's your car");

    CallMain({"Valet"});

    AssertOutput("Here's your car");
}

// Tests 'dumpsys -t 1 service_name' on a service that times out after 2s
TEST_F(DumpsysTest, DumpRunningServiceTimeout) {
    ExpectDumpAndHang("Valet", 2, "Here's your car");

    CallMain({"-t", "1", "Valet"});

    AssertOutputContains("SERVICE 'Valet' DUMP TIMEOUT (1s) EXPIRED");
    AssertNotDumped("Here's your car");

    // Must wait so binder mock is deleted, otherwise test will fail with a leaked object
    sleep(1);
}

// Tests 'dumpsys service_name Y U NO HAVE ARGS' on a service that is running
TEST_F(DumpsysTest, DumpWithArgsRunningService) {
    ExpectDumpWithArgs("SERVICE", {"Y", "U", "NO", "HANDLE", "ARGS"}, "I DO!");

    CallMain({"SERVICE", "Y", "U", "NO", "HANDLE", "ARGS"});

    AssertOutput("I DO!");
}

// Tests 'dumpsys' with no arguments
TEST_F(DumpsysTest, DumpMultipleServices) {
    ExpectListServices({"running1", "stopped2", "running3"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("running3", "dump3");

    CallMain({});

    AssertRunningServices({"running1", "running3"});
    AssertDumped("running1", "dump1");
    AssertStopped("stopped2");
    AssertDumped("running3", "dump3");
}

// Tests 'dumpsys --skip skipped3 skipped5', which should skip these services
TEST_F(DumpsysTest, DumpWithSkip) {
    ExpectListServices({"running1", "stopped2", "skipped3", "running4", "skipped5"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("skipped3", "dump3");
    ExpectDump("running4", "dump4");
    ExpectDump("skipped5", "dump5");

    CallMain({"--skip", "skipped3", "skipped5"});

    AssertRunningServices({"running1", "running4", "skipped3 (skipped)", "skipped5 (skipped)"});
    AssertDumped("running1", "dump1");
    AssertDumped("running4", "dump4");
    AssertStopped("stopped2");
    AssertNotDumped("dump3");
    AssertNotDumped("dump5");
}
