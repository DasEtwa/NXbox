// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/accounts_screen.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <winrt/Windows.Foundation.h>

#include "common/fs/fs.h"
#include "common/scope_exit.h"
#include "core/constants_nxbox.h"
#include "core/hle/service/acc/profile_manager.h"
#include "eden_uwp/accounts.h"
#include "eden_uwp/accounts_policy.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {
using D2D1::RectF;
using winrt::Windows::UI::Core::CoreWindow;
using Manager = Service::Account::ProfileManager;

enum class Page { Question, List, Detail, Presets, Remove, DeleteSaves };

class AccountsScreen {
public:
    AccountsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                   const std::filesystem::path& local_state, bool launch, bool setup_only = false)
        : renderer_(renderer), window_(window), input_(input), local_state_(local_state),
          state_(Accounts::Load(local_state)), setup_only_(setup_only), launch_(launch) {
        page_ = Accounts::NeedsSetup(state_.initialized) ? Page::Question : Page::List;
        if (launch_ && state_.initialized && state_.multi) {
            row_ = static_cast<int>(
                profiles_
                    .GetUserIndex(Common::UUID{state_.selected_uuid.empty()
                                                   ? profiles_.GetUser(0)->RawString()
                                                   : state_.selected_uuid})
                    .value_or(0));
        }
        ReloadImages();
    }

    bool Run() {
        const InputTransition transition(input_);
        const auto token = window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(token);
        };
        Diagnostic("ACCOUNTS open");
        while (!closed_ && !done_) {
            window_.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            Update();
            renderer_.BeginFrame();
            Draw();
            renderer_.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        Diagnostic("ACCOUNTS close");
        return !closed_ && accepted_;
    }
    bool Closed() const {
        return closed_;
    }

private:
    bool Persist() {
        if (!Accounts::Save(local_state_, state_)) {
            failed_ = true;
            return false;
        }
        failed_ = false;
        Accounts::Apply(local_state_);
        return true;
    }

    void ReloadImages() {
        for (std::size_t i = 0; i < profiles_.GetUserCount(); ++i) {
            images_[i] = {};
            Pixels pixels;
            if (renderer_.DecodeImage(ReadFileBytes(Manager::AvatarPath(*profiles_.GetUser(i))),
                                      pixels))
                renderer_.UploadImage(pixels, images_[i]);
        }
        for (std::size_t i = 0; i < presets_.size(); ++i) {
            const auto bytes = Core::Constants::NXBOX_PRESET_AVATARS[i];
            Pixels pixels;
            if (renderer_.DecodeImage(std::vector<u8>(bytes.begin(), bytes.end()), pixels))
                renderer_.UploadImage(pixels, presets_[i]);
        }
    }

    std::wstring Name(std::size_t index) const {
        Service::Account::ProfileBase profile{};
        profiles_.GetProfileBase(index, profile);
        const auto end = std::find(profile.username.begin(), profile.username.end(), 0);
        return Widen(std::string(profile.username.begin(), end));
    }

    void Select() {
        state_.selected_uuid = profiles_.GetUser(account_)->RawString();
        if (Persist()) {
            if (launch_) {
                accepted_ = true;
                done_ = true;
            } else {
                page_ = Page::List;
                row_ = static_cast<int>(account_) + 1;
            }
        }
    }

    void Update() {
        if (entry_) {
            const auto result = entry_->Update(input_);
            if (result == TextEntry::Result::Editing)
                return;
            if (result == TextEntry::Result::Confirmed) {
                const auto name = winrt::to_string(winrt::hstring(entry_->Text()));
                // A Switch username is at most 32 UTF-8 bytes, not 32 codepoints.
                if (name.empty() || name.size() > Service::Account::profile_username_size) {
                    failed_ = true;
                } else if (adding_) {
                    const auto uuid = Common::UUID::MakeRandom();
                    failed_ = profiles_.CreateNewUser(uuid, name).IsError();
                    if (!failed_) {
                        account_ = profiles_.GetUserCount() - 1;
                        failed_ =
                            !profiles_.SetAvatar(uuid, Core::Constants::NXBOX_PRESET_AVATARS[0]);
                        profiles_.WriteUserSaveFile();
                        page_ = Page::Presets;
                        row_ = 0;
                        ReloadImages();
                    }
                } else {
                    Service::Account::ProfileBase profile{};
                    const auto uuid = *profiles_.GetUser(account_);
                    profiles_.GetProfileBase(uuid, profile);
                    profile.username = {};
                    std::copy(name.begin(), name.end(), profile.username.begin());
                    failed_ = !profiles_.SetProfileBase(uuid, profile);
                }
            }
            entry_.reset();
            input_.ConsumeUntilRelease();
            return;
        }
        if (input_.Pressed(Button::B)) {
            if (page_ == Page::Question) {
                row_ = 0;
                if (launch_)
                    AnswerQuestion();
                else
                    page_ = Page::List;
            } else if (page_ == Page::List) {
                done_ = true;
            } else {
                page_ = Page::List;
                row_ = 0;
            }
            return;
        }
        if (page_ == Page::Presets) {
            if (input_.Pressed(Button::Left))
                row_ = std::max(0, row_ - 1);
            if (input_.Pressed(Button::Right))
                row_ = std::min(19, row_ + 1);
            if (input_.Pressed(Button::Up))
                row_ = std::max(0, row_ - 5);
            if (input_.Pressed(Button::Down))
                row_ = std::min(19, row_ + 5);
        } else {
            int count = 2;
            if (page_ == Page::List)
                count =
                    launch_
                        ? static_cast<int>(profiles_.GetUserCount())
                        : (state_.multi
                               ? 1 + static_cast<int>(profiles_.GetUserCount()) +
                                     (profiles_.GetUserCount() < Service::Account::MAX_USERS ? 1
                                                                                             : 0)
                               : 1);
            if (page_ == Page::Detail)
                count = 4;
            if (input_.Pressed(Button::Up) || input_.Pressed(Button::Left))
                row_ = std::max(0, row_ - 1);
            if (input_.Pressed(Button::Down) || input_.Pressed(Button::Right))
                row_ = std::min(count - 1, row_ + 1);
        }
        if (!input_.Pressed(Button::A))
            return;
        switch (page_) {
        case Page::Question:
            AnswerQuestion();
            break;
        case Page::List:
            if (!launch_ && row_ == 0) {
                page_ = Page::Question;
                row_ = 0;
            } else if (!launch_ && row_ > static_cast<int>(profiles_.GetUserCount())) {
                adding_ = true;
                entry_ = std::make_unique<TextEntry>(Tr(Text::AccountName), L"");
            } else {
                account_ = static_cast<std::size_t>(row_ - (launch_ ? 0 : 1));
                if (launch_)
                    Select();
                else {
                    page_ = Page::Detail;
                    row_ = 0;
                }
            }
            break;
        case Page::Detail:
            if (row_ == 0)
                Select();
            else if (row_ == 1) {
                adding_ = false;
                entry_ = std::make_unique<TextEntry>(Tr(Text::AccountName), Name(account_));
            } else if (row_ == 2) {
                page_ = Page::Presets;
                row_ = 0;
            } else if (row_ == 3 && profiles_.GetUserCount() > 1) {
                page_ = Page::Remove;
                row_ = 0;
            }
            break;
        case Page::Presets:
            failed_ = !profiles_.SetAvatar(*profiles_.GetUser(account_),
                                           Core::Constants::NXBOX_PRESET_AVATARS[row_]);
            if (!failed_) {
                ReloadImages();
                page_ = Page::Detail;
                row_ = 2;
            }
            break;
        case Page::Remove:
            page_ = row_ == 1 ? Page::DeleteSaves : Page::Detail;
            row_ = 0;
            break;
        case Page::DeleteSaves: {
            const auto uuid = *profiles_.GetUser(account_);
            if (row_ == 1 && !Accounts::DeleteSaves(uuid.RawString())) {
                failed_ = true;
                break;
            }
            failed_ = !profiles_.RemoveUser(uuid);
            if (!failed_) {
                (void)Common::FS::RemoveFile(Manager::AvatarPath(uuid));
                if (state_.selected_uuid == uuid.RawString())
                    state_.selected_uuid = profiles_.GetUser(0)->RawString();
                Persist();
                ReloadImages();
                page_ = Page::List;
                row_ = 0;
            }
            break;
        }
        }
        input_.ConsumeUntilRelease();
    }

    void AnswerQuestion() {
        state_.initialized = true;
        state_.multi = row_ == 1;
        if (!Persist())
            return;
        row_ = 0;
        page_ = Page::List;
        if (setup_only_ || (launch_ && !state_.multi)) {
            accepted_ = true;
            done_ = true;
        }
    }

    void DrawRow(int row, const std::wstring& label, const Image* image = nullptr) {
        const float y = 285.0f + static_cast<float>(row) * 62.0f;
        const auto rect = RectF(96.0f, y, 1500.0f, y + 54.0f);
        if (image && image->brush)
            renderer_.DrawImage(*image, RectF(108.0f, y, 162.0f, y + 54.0f), 27.0f, 1.0f);
        renderer_.DrawString(label, Font::Nav,
                             RectF(image ? 190.0f : 120.0f, y, 1480.0f, y + 54.0f), Theme::kText,
                             HAlign::Left, VAlign::Middle);
        if (row == row_)
            DrawRing(renderer_, rect, 27.0f, 1.0f);
    }

    void Draw() {
        const Text title = page_ == Page::Question      ? Text::AccountsQuestion
                           : page_ == Page::Remove      ? Text::AccountRemoveQuestion
                           : page_ == Page::DeleteSaves ? Text::AccountDeleteQuestion
                           : page_ == Page::Presets     ? Text::AccountAvatar
                                                        : Text::AccountsTitle;
        renderer_.DrawString(Tr(title), Font::Heading, RectF(96, 115, 1800, 210), Theme::kText);
        if (page_ == Page::Detail || page_ == Page::Remove || page_ == Page::DeleteSaves)
            renderer_.DrawString(Name(account_), Font::Nav, RectF(96, 210, 1800, 270),
                                 Theme::kTextSecondary);
        if (page_ == Page::Question) {
            renderer_.DrawString(Tr(Text::AccountsBody), Font::Body, RectF(96, 210, 1800, 270),
                                 Theme::kTextSecondary);
            DrawRow(0, Tr(Text::AccountsNo));
            DrawRow(1, Tr(Text::AccountsYes));
        } else if (page_ == Page::List) {
            if (!launch_)
                DrawRow(0, Tr(state_.multi ? Text::AccountsMulti : Text::AccountsSingle));
            if (state_.multi) {
                for (std::size_t i = 0; i < profiles_.GetUserCount(); ++i) {
                    auto name = Name(i);
                    if (profiles_.GetUser(i)->RawString() == state_.selected_uuid)
                        name += L" · " + std::wstring(Tr(Text::AccountSelected));
                    DrawRow(static_cast<int>(i) + (launch_ ? 0 : 1), name, &images_[i]);
                }
                if (!launch_ && profiles_.GetUserCount() < Service::Account::MAX_USERS)
                    DrawRow(static_cast<int>(profiles_.GetUserCount()) + 1, Tr(Text::AccountAdd));
            }
        } else if (page_ == Page::Detail) {
            DrawRow(0, Tr(Text::AccountUse));
            DrawRow(1, Tr(Text::AccountRename));
            DrawRow(2, Tr(Text::AccountAvatar));
            DrawRow(3, Tr(profiles_.GetUserCount() > 1 ? Text::AccountRemove : Text::AccountLast));
        } else if (page_ == Page::Presets) {
            for (int i = 0; i < 20; ++i) {
                const float x = 110.0f + (i % 5) * 165.0f;
                const float y = 290.0f + (i / 5) * 150.0f;
                const auto rect = RectF(x, y, x + 128, y + 128);
                renderer_.DrawImage(presets_[i], rect, 64.0f, 1.0f);
                if (i == row_)
                    DrawRing(renderer_, rect, 64.0f, 1.0f);
            }
        } else {
            DrawRow(0, Tr(page_ == Page::Remove ? Text::AccountsNo : Text::AccountKeepSaves));
            DrawRow(1, Tr(page_ == Page::Remove ? Text::AccountRemove : Text::AccountDeleteSaves));
        }
        DrawHints(renderer_, {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                              {Theme::kButtonB, L"B", Tr(Text::HintBack)}});
        if (failed_)
            DrawToastCapsule(renderer_, Tr(Text::AccountFailed), 1.0f);
        if (entry_)
            entry_->Draw(renderer_, Clock::now());
    }

    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    std::filesystem::path local_state_;
    Accounts::State state_;
    Manager profiles_;
    std::array<Image, Service::Account::MAX_USERS> images_;
    std::array<Image, 20> presets_;
    std::unique_ptr<TextEntry> entry_;
    Page page_ = Page::List;
    int row_ = 0;
    std::size_t account_ = 0;
    bool setup_only_ = false;
    bool launch_ = false, adding_ = false, failed_ = false;
    bool closed_ = false, done_ = false, accepted_ = false;
};
} // namespace

bool RunAccountsScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                       const std::filesystem::path& local_state) {
    AccountsScreen screen(renderer, window, input, local_state, false);
    screen.Run();
    return screen.Closed();
}

bool PrepareAccounts(const CoreWindow& window, const std::filesystem::path& local_state,
                     bool choose_for_launch) {
    const auto state = Accounts::Load(local_state);
    if (!Accounts::NeedsPicker(state.initialized, state.multi, choose_for_launch)) {
        Accounts::Apply(local_state);
        return true;
    }
    Renderer renderer;
    renderer.Initialize(window);
    Input input(window);
    AccountsScreen screen(renderer, window, input, local_state, true, !choose_for_launch);
    return screen.Run();
}
} // namespace EdenXbox::Ui
