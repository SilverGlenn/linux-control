#include "EaseOfAccess.h"

const QList<DetailGroup> &easeOfAccessGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::EaseOfAccessCenter,
            {
                { ActionId::LetLinuxSuggestSettings,
                  ActionId::OptimizeVisualDisplay,
                  ActionId::ReplaceSoundsWithVisualCues },
                { ActionId::ChangeHowYourMouseWorks,
                  ActionId::ChangeHowYourKeyboardWorks },
            }
        },
        {
            ActionId::SpeechRecognition,
            {
                { ActionId::StartSpeechRecognition,
                  ActionId::SetUpAMicrophone },
            }
        },
    };
    return groups;
}
