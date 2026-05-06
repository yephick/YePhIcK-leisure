#include <iostream>
#include <vector>
import std;
import llvc.Project;
import llvc.Timeline;
import llvc.EditorController;
import llvc.EditorCommands;

using namespace std;

llvc::IndexedFrameSample marker(int64_t time100ns, bool evaluated = false, bool cleanPoint = false, uint32_t sampleIndex = 0){
    return llvc::IndexedFrameSample{.time100ns=time100ns,.duration100ns=0,.cleanPoint=cleanPoint,.evaluated=evaluated,.sampleIndex=sampleIndex};
}

int main(){
    {
        llvc::Project project{};
        project.timelineDuration100ns(100000000);
        project.frameIndex({marker(20000000,true,true), marker(50000000,true,true)});
        project.refreshSelectedMarkers();
        llvc::EditorHistoryState history{};
        auto r = llvc::executeSetCutBlockCommand(project, history, 10000000, false);
        cout << "setcut changed=" << r.changed << " cuts=" << project.cutScenes().size() << " undo=" << history.undoStack.size() << "\n";
    }
    {
        llvc::Project project{};
        project.timelineDuration100ns(1000);
        project.frameIndex({marker(300,false,false)});
        project.refreshSelectedMarkers();
        project.cutScenes({1});
        llvc::EditorHistoryState history{};
        llvc::Timeline timeline{};
        auto r = llvc::reevaluateClearCutMarkers(project, timeline, {250,400}, history, true);
        cout << "reeval1 changed=" << r.changed << " cuts=";
        for(auto c: project.cutScenes()) cout << c << ",";
        cout << " markers=";
        for(auto &m: project.frameIndex()) cout << "(" << m.time100ns << ",e=" << m.evaluated << ",c=" << m.cleanPoint << ")";
        cout << "\n";
    }
    {
        llvc::Project project{};
        project.timelineDuration100ns(1000);
        project.frameIndex({marker(600,false,false)});
        project.refreshSelectedMarkers();
        project.cutScenes({1});
        llvc::EditorHistoryState history{};
        llvc::Timeline timeline{};
        auto r = llvc::reevaluateClearCutMarkers(project, timeline, {550,700,900}, history, false);
        cout << "reeval2 changed=" << r.changed << " cuts=";
        for(auto c: project.cutScenes()) cout << c << ",";
        cout << " markers=";
        for(auto &m: project.frameIndex()) cout << "(" << m.time100ns << ",e=" << m.evaluated << ",c=" << m.cleanPoint << ")";
        cout << "\n";
    }
}
