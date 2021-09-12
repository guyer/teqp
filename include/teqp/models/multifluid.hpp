#pragma once

#include "nlohmann/json.hpp"

#include <Eigen/Dense>
#include <fstream>
#include <string>
#include <cmath>
#include <optional>
#include <variant>

#include "teqp/derivs.hpp"
#include "teqp/types.hpp"
#include "teqp/constants.hpp"

#include "MultiComplex/MultiComplex.hpp"

#include "multifluid_eosterms.hpp"

// See https://eigen.tuxfamily.org/dox/TopicCustomizing_CustomScalar.html
namespace Eigen {
    template<typename TN> struct NumTraits<mcx::MultiComplex<TN>> : NumTraits<double> // permits to get the epsilon, dummy_precision, lowest, highest functions
    {
        enum {
            IsComplex = 1,
            IsInteger = 0,
            IsSigned = 1,
            RequireInitialization = 1,
            ReadCost = 1,
            AddCost = 3,
            MulCost = 3
        };
    };
}

template<typename EOSCollection>
class CorrespondingStatesContribution {

private:
    const EOSCollection EOSs;
public:
    CorrespondingStatesContribution(EOSCollection&& EOSs) : EOSs(EOSs) {};

    template<typename TauType, typename DeltaType, typename MoleFractions>
    auto alphar(const TauType& tau, const DeltaType& delta, const MoleFractions& molefracs) const {
        using resulttype = std::common_type_t<decltype(tau), decltype(molefracs[0]), decltype(delta)>; // Type promotion, without the const-ness
        resulttype alphar = 0.0;
        auto N = molefracs.size();
        for (auto i = 0; i < N; ++i) {
            alphar = alphar + molefracs[i] * EOSs[i].alphar(tau, delta);
        }
        return alphar;
    }

    template<typename TauType, typename DeltaType>
    auto alphari(const TauType& tau, const DeltaType& delta, std::size_t i) const {
        using resulttype = std::common_type_t<decltype(tau), decltype(delta)>; // Type promotion, without the const-ness
        return EOSs[i].alphar(tau, delta);
    }

    auto get_EOS(std::size_t i) const{
        return EOSs[i];
    }
};

template<typename FCollection, typename DepartureFunctionCollection>
class DepartureContribution {

private:
    const FCollection F;
    const DepartureFunctionCollection funcs;
public:
    DepartureContribution(FCollection&& F, DepartureFunctionCollection&& funcs) : F(F), funcs(funcs) {};

    template<typename TauType, typename DeltaType, typename MoleFractions>
    auto alphar(const TauType& tau, const DeltaType& delta, const MoleFractions& molefracs) const {
        using resulttype = std::common_type_t<decltype(tau), decltype(molefracs[0]), decltype(delta)>; // Type promotion, without the const-ness
        resulttype alphar = 0.0;
        auto N = molefracs.size();
        for (auto i = 0; i < N; ++i) {
            for (auto j = i+1; j < N; ++j) {
                alphar = alphar + molefracs[i] * molefracs[j] * F(i, j) * funcs[i][j].alphar(tau, delta);
            }
        }
        return alphar;
    }
};

template<typename ReducingFunction, typename CorrespondingTerm, typename DepartureTerm>
class MultiFluid {  

private:
    std::string meta = ""; ///< A string that can be used to store arbitrary metadata as needed
public:
    const ReducingFunction redfunc;
    const CorrespondingTerm corr;
    const DepartureTerm dep;

    template<class VecType>
    auto R(const VecType& molefrac) const {
        return get_R_gas<decltype(molefrac[0])>();
    }

    /// Store some sort of metadata in string form (perhaps a JSON representation of the model?)
    void set_meta(const std::string& m) { meta = m; }
    /// Get the metadata stored in string form
    auto get_meta() const { return meta; }

    MultiFluid(ReducingFunction&& redfunc, CorrespondingTerm&& corr, DepartureTerm&& dep) : redfunc(redfunc), corr(corr), dep(dep) {};

    template<typename TType, typename RhoType>
    auto alphar(TType T,
        const RhoType& rhovec,
        const std::optional<typename RhoType::value_type> rhotot = std::nullopt) const
    {
        typename RhoType::value_type rhotot_ = (rhotot.has_value()) ? rhotot.value() : std::accumulate(std::begin(rhovec), std::end(rhovec), (decltype(rhovec[0]))0.0);
        auto molefrac = rhovec / rhotot_;
        return alphar(T, rhotot_, molefrac);
    }

    template<typename TType, typename RhoType, typename MoleFracType>
    auto alphar(const TType &T,
        const RhoType &rho,
        const MoleFracType& molefrac) const
    {
        auto Tred = forceeval(redfunc.get_Tr(molefrac));
        auto rhored = forceeval(redfunc.get_rhor(molefrac));
        auto delta = forceeval(rho / rhored);
        auto tau = forceeval(Tred / T);
        auto val = corr.alphar(tau, delta, molefrac) + dep.alphar(tau, delta, molefrac);
        return forceeval(val);
    }
};


class MultiFluidReducingFunction {
private:
    Eigen::MatrixXd YT, Yv;

    template <typename Num>
    auto cube(Num x) const {
        return x*x*x;
    }
    template <typename Num>
    auto square(Num x) const {
        return x*x;
    }

public:
    const Eigen::MatrixXd betaT, gammaT, betaV, gammaV;
    const Eigen::ArrayXd Tc, vc;

    template<typename ArrayLike>
    MultiFluidReducingFunction(
        const Eigen::MatrixXd& betaT, const Eigen::MatrixXd& gammaT,
        const Eigen::MatrixXd& betaV, const Eigen::MatrixXd& gammaV,
        const ArrayLike& Tc, const ArrayLike& vc)
        : betaT(betaT), gammaT(gammaT), betaV(betaV), gammaV(gammaV), Tc(Tc), vc(vc) {

        auto N = Tc.size();

        YT.resize(N, N); YT.setZero();
        Yv.resize(N, N); Yv.setZero();
        for (auto i = 0; i < N; ++i) {
            for (auto j = i + 1; j < N; ++j) {
                YT(i, j) = betaT(i, j) * gammaT(i, j) * sqrt(Tc[i] * Tc[j]);
                YT(j, i) = betaT(j, i) * gammaT(j, i) * sqrt(Tc[i] * Tc[j]);
                Yv(i, j) = 1.0 / 8.0 * betaV(i, j) * gammaV(i, j) * cube(cbrt(vc[i]) + cbrt(vc[j]));
                Yv(j, i) = 1.0 / 8.0 * betaV(j, i) * gammaV(j, i) * cube(cbrt(vc[i]) + cbrt(vc[j]));
            }
        }
    }

    template <typename MoleFractions>
    auto Y(const MoleFractions& z, const Eigen::ArrayXd& Yc, const Eigen::MatrixXd& beta, const Eigen::MatrixXd& Yij) const {

        auto N = z.size();
        typename MoleFractions::value_type sum1 = 0.0;
        for (auto i = 0; i < N; ++i) {
            sum1 = sum1 + square(z[i]) * Yc[i];
        }
        
        typename MoleFractions::value_type sum2 = 0.0;
        for (auto i = 0; i < N-1; ++i){
            for (auto j = i+1; j < N; ++j) {
                sum2 = sum2 + 2.0*z[i]*z[j]*(z[i] + z[j])/(square(beta(i, j))*z[i] + z[j])*Yij(i, j);
            }
        }

        return sum1 + sum2;
    }

    static auto get_BIPdep(const nlohmann::json& collection, const std::vector<std::string>& components, const nlohmann::json& flags) {

        if (flags.contains("estimate")) {
            return nlohmann::json({
                {"betaT", 1.0}, {"gammaT", 1.0}, {"betaV", 1.0}, {"gammaV", 1.0}, {"F", 0.0} 
            });
        }

        // convert string to upper case
        auto toupper = [](const std::string s){ auto data = s; std::for_each(data.begin(), data.end(), [](char& c) { c = ::toupper(c); }); return data;};

        std::string comp0 = toupper(components[0]);
        std::string comp1 = toupper(components[1]);
        for (auto& el : collection) {
            std::string name1 = toupper(el["Name1"]);
            std::string name2 = toupper(el["Name2"]);
            if (comp0 == name1 && comp1 == name2) {
                return el;
            }
            if (comp0 == name2 && comp1 == name1) {
                return el;
            }
        }
        throw std::invalid_argument("Can't match this binary pair");
    }
    static auto get_binary_interaction_double(const nlohmann::json& collection, const std::vector<std::string>& components, const nlohmann::json& flags) {
        auto el = get_BIPdep(collection, components, flags);

        double betaT = el["betaT"], gammaT = el["gammaT"], betaV = el["betaV"], gammaV = el["gammaV"];
        // Backwards order of components, flip beta values
        if (components[0] == el["Name2"] && components[1] == el["Name1"]) {
            betaT = 1.0 / betaT;
            betaV = 1.0 / betaV;
        }
        return std::make_tuple(betaT, gammaT, betaV, gammaV);
    }
    static auto get_BIP_matrices(const nlohmann::json& collection, const std::vector<std::string>& components, const nlohmann::json& flags) {
        Eigen::MatrixXd betaT, gammaT, betaV, gammaV, YT, Yv;
        auto N = components.size();
        betaT.resize(N, N); betaT.setZero();
        gammaT.resize(N, N); gammaT.setZero();
        betaV.resize(N, N); betaV.setZero();
        gammaV.resize(N, N); gammaV.setZero();
        for (auto i = 0; i < N; ++i) {
            for (auto j = i + 1; j < N; ++j) {
                auto [betaT_, gammaT_, betaV_, gammaV_] = get_binary_interaction_double(collection, { components[i], components[j] }, flags);
                betaT(i, j) = betaT_;         betaT(j, i) = 1.0 / betaT(i, j);
                gammaT(i, j) = gammaT_;       gammaT(j, i) = gammaT(i, j);
                betaV(i, j) = betaV_;         betaV(j, i) = 1.0 / betaV(i, j);
                gammaV(i, j) = gammaV_;       gammaV(j, i) = gammaV(i, j);
            }
        }
        return std::make_tuple(betaT, gammaT, betaV, gammaV);
    }
    static auto get_Tcvc(const std::string& coolprop_root, const std::vector<std::string>& components) {
        Eigen::ArrayXd Tc(components.size()), vc(components.size());
        using namespace nlohmann;
        auto i = 0;
        for (auto& c : components) {
            std::string path = coolprop_root + "/dev/fluids/" + c + ".json";
            std::ifstream ifs(path);
            if (!ifs) {
                throw std::invalid_argument("Load path is invalid: " + path);
            }
            auto j = json::parse(ifs);
            auto red = j["EOS"][0]["STATES"]["reducing"];
            double Tc_ = red["T"];
            double rhoc_ = red["rhomolar"];
            Tc[i] = Tc_;
            vc[i] = 1.0 / rhoc_;
            i++;
        }
        return std::make_tuple(Tc, vc);
    }
    static auto get_F_matrix(const nlohmann::json& collection, const std::vector<std::string>& components, const nlohmann::json& flags) {
        Eigen::MatrixXd F(components.size(), components.size());
        auto N = components.size();
        for (auto i = 0; i < N; ++i) {
            F(i, i) = 0.0;
            for (auto j = i + 1; j < N; ++j) {
                auto el = get_BIPdep(collection, { components[i], components[j] }, flags);
                if (el.empty()) {
                    F(i, j) = 0.0;
                    F(j, i) = 0.0;
                }
                else{
                    F(i, j) = el["F"];
                    F(j, i) = el["F"];
                }   
            }
        }
        return F;
    }
    template<typename MoleFractions> auto get_Tr(const MoleFractions& molefracs) const { return Y(molefracs, Tc, betaT, YT); }
    template<typename MoleFractions> auto get_rhor(const MoleFractions& molefracs) const { return 1.0 / Y(molefracs, vc, betaV, Yv); }
};

class MultiFluidInvariantReducingFunction {
private:
    Eigen::MatrixXd YT, Yv;
    template <typename Num> auto cube(Num x) const { return x * x * x; }
    template <typename Num> auto square(Num x) const { return x * x; }
public:
    const Eigen::MatrixXd phiT, lambdaT, phiV, lambdaV;
    const Eigen::ArrayXd Tc, vc;

    template<typename ArrayLike>
    MultiFluidInvariantReducingFunction(
        const Eigen::MatrixXd& phiT, const Eigen::MatrixXd& lambdaT,
        const Eigen::MatrixXd& phiV, const Eigen::MatrixXd& lambdaV,
        const ArrayLike& Tc, const ArrayLike& vc)
        : phiT(phiT), lambdaT(lambdaT), phiV(phiV), lambdaV(lambdaV), Tc(Tc), vc(vc) {

        auto N = Tc.size();

        YT.resize(N, N); YT.setZero();
        Yv.resize(N, N); Yv.setZero();
        for (auto i = 0; i < N; ++i) {
            for (auto j = 0; j < N; ++j) {
                YT(i, j) = sqrt(Tc[i] * Tc[j]);
                YT(j, i) = sqrt(Tc[i] * Tc[j]);
                Yv(i, j) = 1.0 / 8.0 * cube(cbrt(vc[i]) + cbrt(vc[j]));
                Yv(j, i) = 1.0 / 8.0 * cube(cbrt(vc[i]) + cbrt(vc[j]));
            }
        }
    }
    template <typename MoleFractions>
    auto Y(const MoleFractions& z, const Eigen::MatrixXd& phi, const Eigen::MatrixXd& lambda, const Eigen::MatrixXd& Yij) const {
        auto N = z.size();
        typename MoleFractions::value_type sum = 0.0;
        for (auto i = 0; i < N; ++i) {
            for (auto j = 0; j < N; ++j) {
                auto contrib = z[i] * z[j] * (phi(i, j) + z[j] * lambda(i, j)) * Yij(i, j);
                sum += contrib;
            }
        }
        return sum;
    }
    template<typename MoleFractions> auto get_Tr(const MoleFractions& molefracs) const { return Y(molefracs, phiT, lambdaT, YT); }
    template<typename MoleFractions> auto get_rhor(const MoleFractions& molefracs) const { return 1.0 / Y(molefracs, phiV, lambdaV, Yv); }
};

inline auto build_departure_function(const nlohmann::json& j) {
    auto build_power = [&](auto term) {
        std::size_t N = term["n"].size();

        PowerEOSTerm eos;

        auto eigorzero = [&term, &N](const std::string& name) -> Eigen::ArrayXd {
            if (!term[name].empty()) {
                return toeig(term[name]);
            }
            else {
                return Eigen::ArrayXd::Zero(N);
            }
        };


        eos.n = eigorzero("n");
        eos.t = eigorzero("t");
        eos.d = eigorzero("d");

        Eigen::ArrayXd c(N), l(N); c.setZero();
        if (term["l"].empty()) {
            // exponential part not included
            l.setZero();
            if (!all_same_length(term, { "n","t","d" })) {
                throw std::invalid_argument("Lengths are not all identical in polynomial term");
            }
        }
        else {
            if (!all_same_length(term, { "n","t","d","l"})) {
                throw std::invalid_argument("Lengths are not all identical in exponential term");
            }
            l = toeig(term["l"]);
            // l is included, use it to build c; c_i = 1 if l_i > 0, zero otherwise
            for (auto i = 0; i < c.size(); ++i) {
                if (l[i] > 0) {
                    c[i] = 1.0;
                }
            }
        }
        eos.c = c;
        eos.l = l;

        eos.l_i = eos.l.cast<int>();

        if (((eos.l_i.cast<double>() - eos.l).cwiseAbs() > 0.0).any()) {
            throw std::invalid_argument("Non-integer entry in l found");
        }

        return eos;
    };

    auto build_gaussian = [&](auto& term) {
        GaussianEOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.t = toeig(term["t"]);
        eos.d = toeig(term["d"]);
        eos.eta = toeig(term["eta"]);
        eos.beta = toeig(term["beta"]);
        eos.gamma = toeig(term["gamma"]);
        eos.epsilon = toeig(term["epsilon"]);
        if (!all_same_length(term, { "n","t","d","eta","beta","gamma","epsilon" })) {
            throw std::invalid_argument("Lengths are not all identical in Gaussian term");
        }
        return eos;
    };
    auto build_GERG2004 = [&](const auto& term, auto& dep) {
        if (!all_same_length(term, { "n","t","d","eta","beta","gamma","epsilon" })) {
            throw std::invalid_argument("Lengths are not all identical in GERG term");
        }
        int Npower = term["Npower"];
        auto NGERG = static_cast<int>(term["n"].size()) - Npower;

        PowerEOSTerm eos;
        eos.n = toeig(term["n"]).head(Npower);
        eos.t = toeig(term["t"]).head(Npower);
        eos.d = toeig(term["d"]).head(Npower);
        if (term.contains("l")) {
            eos.l = toeig(term["l"]).head(Npower);
        }
        else {
            eos.l = 0.0 * eos.n;
        }
        eos.c = (eos.l > 0).cast<int>().cast<double>();
        eos.l_i = eos.l.cast<int>();
        dep.add_term(eos);

        GERG2004EOSTerm e;
        e.n = toeig(term["n"]).tail(NGERG);
        e.t = toeig(term["t"]).tail(NGERG);
        e.d = toeig(term["d"]).tail(NGERG);
        e.eta = toeig(term["eta"]).tail(NGERG);
        e.beta = toeig(term["beta"]).tail(NGERG);
        e.gamma = toeig(term["gamma"]).tail(NGERG);
        e.epsilon = toeig(term["epsilon"]).tail(NGERG);
        dep.add_term(e);
    };
    auto build_GaussianExponential = [&](const auto& term, auto& dep) {
        if (!all_same_length(term, { "n","t","d","eta","beta","gamma","epsilon" })) {
            throw std::invalid_argument("Lengths are not all identical in Gaussian+Exponential term");
        }
        int Npower = term["Npower"];
        auto NGauss = static_cast<int>(term["n"].size()) - Npower;

        PowerEOSTerm eos;
        eos.n = toeig(term["n"]).head(Npower);
        eos.t = toeig(term["t"]).head(Npower);
        eos.d = toeig(term["d"]).head(Npower);
        if (term.contains("l")) {
            eos.l = toeig(term["l"]).head(Npower);
        }
        else {
            eos.l = 0.0 * eos.n;
        }
        eos.c = (eos.l > 0).cast<int>().cast<double>();
        eos.l_i = eos.l.cast<int>();
        dep.add_term(eos);

        GaussianEOSTerm e;
        e.n = toeig(term["n"]).tail(NGauss);
        e.t = toeig(term["t"]).tail(NGauss);
        e.d = toeig(term["d"]).tail(NGauss);
        e.eta = toeig(term["eta"]).tail(NGauss);
        e.beta = toeig(term["beta"]).tail(NGauss);
        e.gamma = toeig(term["gamma"]).tail(NGauss);
        e.epsilon = toeig(term["epsilon"]).tail(NGauss);
        dep.add_term(e);
    };

    std::string type = j["type"];
    DepartureTerms dep;
    if (type == "Exponential") {
        dep.add_term(build_power(j));
    }
    else if (type == "GERG-2004" || type == "GERG-2008") {
        build_GERG2004(j, dep);
    }
    else if (type == "Gaussian+Exponential") {
        build_GaussianExponential(j, dep);
    }
    else if (type == "none") {
        dep.add_term(NullEOSTerm());
    }
    else {
        throw std::invalid_argument("Bad departure term type: " + type);
    }
    return dep;
}

inline auto get_departure_function_matrix(const std::string& coolprop_root, const nlohmann::json& BIPcollection, const std::vector<std::string>& components, const nlohmann::json& flags) {

    // Allocate the matrix with default models
    std::vector<std::vector<DepartureTerms>> funcs(components.size()); for (auto i = 0; i < funcs.size(); ++i) { funcs[i].resize(funcs.size()); }

    // Load the collection of data on departure functions
    auto depcollection = nlohmann::json::parse(std::ifstream(coolprop_root + "/dev/mixtures/mixture_departure_functions.json"));
    auto get_departure_json = [&depcollection](const std::string& Name) {
        for (auto& el : depcollection) {
            if (el["Name"] == Name) { return el; }
        }
        throw std::invalid_argument("Bad argument");
    };

    for (auto i = 0; i < funcs.size(); ++i) {
        for (auto j = i + 1; j < funcs.size(); ++j) {
            auto BIP = MultiFluidReducingFunction::get_BIPdep(BIPcollection, { components[i], components[j] }, flags);
            std::string funcname = BIP.contains("function") ? BIP["function"] : "";
            if (!funcname.empty()) {
                auto jj = get_departure_json(funcname);
                funcs[i][j] = build_departure_function(jj);
                funcs[j][i] = build_departure_function(jj);
            }
            else {
                funcs[i][j].add_term(NullEOSTerm());
                funcs[j][i].add_term(NullEOSTerm());
            }
        }
    }
    return funcs;
}

inline auto get_EOS_terms(const std::string& coolprop_root, const std::string& name)
{
    using namespace nlohmann;
    auto j = json::parse(std::ifstream(coolprop_root + "/dev/fluids/" + name + ".json"));
    auto alphar = j["EOS"][0]["alphar"];

    const std::vector<std::string> allowed_types = { "ResidualHelmholtzPower", "ResidualHelmholtzGaussian", "ResidualHelmholtzNonAnalytic","ResidualHelmholtzGaoB", "ResidualHelmholtzLemmon2005", "ResidualHelmholtzExponential" };

    auto isallowed = [&](const auto& conventional_types, const std::string& name) {
        for (auto& a : conventional_types) { if (name == a) { return true; }; } return false;
    };

    for (auto& term : alphar) {
        std::string type = term["type"];
        if (!isallowed(allowed_types, type)) {
            std::string a = allowed_types[0]; for (auto i = 1; i < allowed_types.size(); ++i) { a += "," + allowed_types[i]; }
            throw std::invalid_argument("Bad type:" + type + "; allowed types are: {" + a + "}");
        }
    }

    EOSTerms container;

    auto build_power = [&](auto term) {
        std::size_t N = term["n"].size();

        PowerEOSTerm eos;

        auto eigorzero = [&term, &N](const std::string& name) -> Eigen::ArrayXd {
            if (!term[name].empty()) {
                return toeig(term[name]);
            }
            else {
                return Eigen::ArrayXd::Zero(N);
            }
        };


        eos.n = eigorzero("n");
        eos.t = eigorzero("t");
        eos.d = eigorzero("d");

        Eigen::ArrayXd c(N), l(N); c.setZero();
        if (term["l"].empty()) {
            // exponential part not included
            l.setZero();
        }
        else {
            l = toeig(term["l"]);
            // l is included, use it to build c; c_i = 1 if l_i > 0, zero otherwise
            for (auto i = 0; i < c.size(); ++i) {
                if (l[i] > 0) {
                    c[i] = 1.0;
                }
            }
        }
        eos.c = c;
        eos.l = l;

        eos.l_i = eos.l.cast<int>();

        if (((eos.l_i.cast<double>() - eos.l).cwiseAbs() > 0.0).any()) {
            throw std::invalid_argument("Non-integer entry in l found");
        }
        
        return eos;
    };

    auto build_Lemmon2005 = [&](auto term) {
        Lemmon2005EOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.t = toeig(term["t"]);
        eos.d = toeig(term["d"]);
        eos.m = toeig(term["m"]);
        eos.l = toeig(term["l"]);
        eos.l_i = eos.l.cast<int>();
        if (!all_same_length(term, { "n","t","d","m","l" })) {
            throw std::invalid_argument("Lengths are not all identical in Lemmon2005 term");
        }
        if (((eos.l_i.cast<double>() - eos.l).cwiseAbs() > 0.0).any()) {
            throw std::invalid_argument("Non-integer entry in l found");
        }
        return eos;
    };

    auto build_gaussian = [&](auto term) {
        GaussianEOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.t = toeig(term["t"]);
        eos.d = toeig(term["d"]);
        eos.eta = toeig(term["eta"]);
        eos.beta = toeig(term["beta"]);
        eos.gamma = toeig(term["gamma"]);
        eos.epsilon = toeig(term["epsilon"]);
        if (!all_same_length(term, { "n","t","d","eta","beta","gamma","epsilon" })) {
            throw std::invalid_argument("Lengths are not all identical in Gaussian term");
        }
        return eos;
    };

    auto build_exponential = [&](auto term) {
        ExponentialEOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.t = toeig(term["t"]);
        eos.d = toeig(term["d"]);
        eos.g = toeig(term["g"]);
        eos.l = toeig(term["l"]);
        eos.l_i = eos.l.cast<int>();
        if (!all_same_length(term, { "n","t","d","g","l" })) {
            throw std::invalid_argument("Lengths are not all identical in exponential term");
        }
        return eos;
    };

    auto build_GaoB = [&](auto term) {
        GaoBEOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.t = toeig(term["t"]);
        eos.d = toeig(term["d"]);
        eos.eta = -toeig(term["eta"]); // Watch out for this sign flip!!
        eos.beta = toeig(term["beta"]);
        eos.gamma = toeig(term["gamma"]);
        eos.epsilon = toeig(term["epsilon"]);
        eos.b = toeig(term["b"]);
        if (!all_same_length(term, { "n","t","d","eta","beta","gamma","epsilon","b" })) {
            throw std::invalid_argument("Lengths are not all identical in GaoB term");
        }
        return eos;
    };

    /// lambda function for adding non-analytic terms
    auto build_na = [&](auto& term) {
        NonAnalyticEOSTerm eos;
        eos.n = toeig(term["n"]);
        eos.A = toeig(term["A"]);
        eos.B = toeig(term["B"]);
        eos.C = toeig(term["C"]);
        eos.D = toeig(term["D"]);
        eos.a = toeig(term["a"]);
        eos.b = toeig(term["b"]);
        eos.beta = toeig(term["beta"]);
        if (!all_same_length(term, { "n","A","B","C","D","a","b","beta" })) {
            throw std::invalid_argument("Lengths are not all identical in nonanalytic term");
        }
        return eos;
    };
    
    for (auto& term : alphar) {
        std::string type = term["type"];
        if (type == "ResidualHelmholtzPower") {
            container.add_term(build_power(term));
        }
        else if (type == "ResidualHelmholtzGaussian") {
            container.add_term(build_gaussian(term));
        }
        else if (type == "ResidualHelmholtzNonAnalytic") {
            container.add_term(build_na(term));
        }
        else if (type == "ResidualHelmholtzLemmon2005") {
            container.add_term(build_Lemmon2005(term));
        }
        else if (type == "ResidualHelmholtzGaoB") {
            container.add_term(build_GaoB(term));
        }
        else if (type == "ResidualHelmholtzExponential") {
            container.add_term(build_exponential(term));
        }
        else {
            throw std::invalid_argument("Bad term type: "+type);
        }
    }
    return container;
}

inline auto get_EOSs(const std::string& coolprop_root, const std::vector<std::string>& names) {
    std::vector<EOSTerms> EOSs;
    for (auto& name : names) {
        auto term = get_EOS_terms(coolprop_root, name);
        EOSs.emplace_back(term);
    }
    return EOSs;
}

inline auto build_multifluid_model(const std::vector<std::string>& components, const std::string& coolprop_root, const std::string& BIPcollectionpath, const nlohmann::json& flags = {}) {

    const auto BIPcollection = nlohmann::json::parse(std::ifstream(BIPcollectionpath));

    // Pure fluids
    auto [Tc, vc] = MultiFluidReducingFunction::get_Tcvc(coolprop_root, components);
    auto EOSs = get_EOSs(coolprop_root, components); 
    
    // Things related to the mixture
    auto F = MultiFluidReducingFunction::get_F_matrix(BIPcollection, components, flags);
    auto funcs = get_departure_function_matrix(coolprop_root, BIPcollection, components, flags);
    auto [betaT, gammaT, betaV, gammaV] = MultiFluidReducingFunction::get_BIP_matrices(BIPcollection, components, flags);

    auto redfunc = MultiFluidReducingFunction(betaT, gammaT, betaV, gammaV, Tc, vc);

    return MultiFluid(
        std::move(redfunc),
        std::move(CorrespondingStatesContribution(std::move(EOSs))),
        std::move(DepartureContribution(std::move(F), std::move(funcs)))
    );
}

/**
This class holds a lightweight reference to the core parts of the model

The reducing and departure functions are moved into this class, while the donor class is used for the corresponding states portion
*/
template<typename ReducingFunction, typename DepartureFunction, typename BaseClass>
class MultiFluidAdapter {

private:
    std::string meta = "";

public:
    const BaseClass& base;
    const ReducingFunction redfunc;
    const DepartureFunction depfunc;

    template<class VecType>
    auto R(const VecType& molefrac) const { return base.R(molefrac); }

    MultiFluidAdapter(const BaseClass& base, ReducingFunction&& redfunc, DepartureFunction &&depfunc) : base(base), redfunc(redfunc), depfunc(depfunc) {};

    /// Store some sort of metadata in string form (perhaps a JSON representation of the model?)
    void set_meta(const std::string& m) { meta = m; }
    /// Get the metadata stored in string form
    auto get_meta() const { return meta; }

    template<typename TType, typename RhoType, typename MoleFracType>
    auto alphar(const TType& T,
        const RhoType& rho,
        const MoleFracType& molefrac) const
    {
        auto Tred = forceeval(redfunc.get_Tr(molefrac));
        auto rhored = forceeval(redfunc.get_rhor(molefrac));
        auto delta = forceeval(rho / rhored);
        auto tau = forceeval(Tred / T);
        auto val = base.corr.alphar(tau, delta, molefrac) + depfunc.alphar(tau, delta, molefrac);
        return forceeval(val);
    }
};

template<class Model>
auto build_multifluid_mutant(Model& model, const nlohmann::json& jj) {

    auto red = model.redfunc;
    auto N = red.Tc.size();

    auto betaT = red.betaT, gammaT = red.gammaT, betaV = red.betaV, gammaV = red.gammaV;
    auto Tc = red.Tc, vc = red.vc;

    // Allocate the matrices of default models and F factors
    Eigen::MatrixXd F(N, N); F.setZero();
    std::vector<std::vector<DepartureTerms>> funcs(N);
    for (auto i = 0; i < N; ++i) { funcs[i].resize(N); }

    for (auto i = 0; i < N; ++i) {
        for (auto j = i; j < N; ++j) {
            if (i == j) {
                funcs[i][i].add_term(NullEOSTerm());
            }
            else {
                // Extract the given entry
                auto entry = jj[std::to_string(i)][std::to_string(j)];

                auto BIP = entry["BIP"];
                // Set the reducing function parameters in the copy
                betaT(i, j) = BIP.at("betaT");
                betaT(j, i) = 1 / red.betaT(i, j);
                betaV(i, j) = BIP.at("betaV");
                betaV(j, i) = 1 / red.betaV(i, j);
                gammaT(i, j) = BIP.at("gammaT"); gammaT(j, i) = gammaT(i, j);
                gammaV(i, j) = BIP.at("gammaV"); gammaV(j, i) = gammaV(i, j);

                // Build the matrix of F and departure functions
                auto dep = entry["departure"];
                F(i, j) = BIP.at("Fij");
                F(j, i) = F(i, j);
                funcs[i][j] = build_departure_function(dep);
                funcs[j][i] = build_departure_function(dep);
            }
        }
    }

    auto newred = MultiFluidReducingFunction(betaT, gammaT, betaV, gammaV, Tc, vc);
    auto newdep = DepartureContribution(std::move(F), std::move(funcs));
    auto mfa = MultiFluidAdapter(model, std::move(newred), std::move(newdep));
    /// Store the departure term in the adapted multifluid class
    mfa.set_meta(jj.dump());
    return mfa;
}

template<typename Model>
auto build_multifluid_mutant_invariant(Model& model, const nlohmann::json& jj) {

    auto red = model.redfunc;
    auto N = red.Tc.size();
    if (N != 2) {
        throw std::invalid_argument("Only binary mixtures are currently supported with invariant departure functions");
    }

    auto phiT = red.betaT, lambdaT = red.gammaT, phiV = red.betaV, lambdaV = red.gammaV;
    phiT.setOnes(); phiV.setOnes();
    lambdaV.setZero(); lambdaV.setZero();
    auto Tc = red.Tc, vc = red.vc;

    // Allocate the matrices of default models and F factors
    Eigen::MatrixXd F(N, N); F.setZero();
    std::vector<std::vector<DepartureTerms>> funcs(N);
    for (auto i = 0; i < N; ++i) { funcs[i].resize(N); }

    for (auto i = 0; i < N; ++i) {
        for (auto j = i; j < N; ++j) {
            if (i == j) {
                funcs[i][i].add_term(NullEOSTerm());
            }
            else {
                // Extract the given entry
                auto entry = jj[std::to_string(i)][std::to_string(j)];

                auto BIP = entry["BIP"];
                // Set the reducing function parameters in the copy
                phiT(i, j) = BIP.at("phiT");
                phiT(j, i) = phiT(i, j);
                lambdaT(i, j) = BIP.at("lambdaT"); 
                lambdaT(j, i) = -lambdaT(i, j);

                phiV(i, j) = BIP.at("phiV");
                phiV(j, i) = phiV(i, j);
                lambdaV(i, j) = BIP.at("lambdaV"); 
                lambdaV(j, i) = -lambdaV(i, j);

                // Build the matrix of F and departure functions
                auto dep = entry["departure"];
                F(i, j) = BIP.at("Fij");
                F(j, i) = F(i, j);
                funcs[i][j] = build_departure_function(dep);
                funcs[j][i] = build_departure_function(dep);
            }
        }
    }

    auto newred = MultiFluidInvariantReducingFunction(phiT, lambdaT, phiV, lambdaV, Tc, vc);
    auto newdep = DepartureContribution(std::move(F), std::move(funcs));
    auto mfa = MultiFluidAdapter(model, std::move(newred), std::move(newdep));
    /// Store the departure term in the adapted multifluid class
    mfa.set_meta(jj.dump());
    return mfa;
}


class DummyEOS {
public:
    template<typename TType, typename RhoType> auto alphar(TType tau, const RhoType& delta) const { return tau * delta; }
};
class DummyReducingFunction {
public:
    template<typename MoleFractions> auto get_Tr(const MoleFractions& molefracs) const { return molefracs[0]; }
    template<typename MoleFractions> auto get_rhor(const MoleFractions& molefracs) const { return molefracs[0]; }
};
inline auto build_dummy_multifluid_model(const std::vector<std::string>& components) {
    std::vector<DummyEOS> EOSs(2);
    std::vector<std::vector<DummyEOS>> funcs(2); for (auto i = 0; i < funcs.size(); ++i) { funcs[i].resize(funcs.size()); }
    std::vector<std::vector<double>> F(2); for (auto i = 0; i < F.size(); ++i) { F[i].resize(F.size()); }

    struct Fwrapper {
    private: 
        const std::vector<std::vector<double>> F_;
    public:
        Fwrapper(const std::vector<std::vector<double>> &F) : F_(F){};
        auto operator ()(std::size_t i, std::size_t j) const{ return F_[i][j]; }
    };
    auto ff = Fwrapper(F);
    auto redfunc = DummyReducingFunction();
    return MultiFluid(std::move(redfunc), std::move(CorrespondingStatesContribution(std::move(EOSs))), std::move(DepartureContribution(std::move(ff), std::move(funcs))));
}
inline void test_dummy() {
    auto model = build_dummy_multifluid_model({ "A", "B" });
    std::valarray<double> rhovec = { 1.0, 2.0 };
    auto alphar = model.alphar(300.0, rhovec);
}